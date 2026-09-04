/*
 * bench_parse.c - the number the SIMD decision rests on.
 *
 * Phase 0's benchmark measured a byte counter, which the compiler auto
 * vectorizes, and reported 2776 MB/s. That was only ever an upper bound on the
 * ingest path. The RFC 4180 state machine has data dependent branching and
 * cannot vectorize the same way, so this replaces it with the real figure.
 *
 * The gate from spec section 11 and 3.3: a scalar table driven parser was
 * predicted at 300 to 500 MB/s, which already clears the under one second
 * requirement for 50 MB by a wide margin. At or above 300 MB/s, SIMD stays in
 * Phase 7 as a batch CPU cost lever. Below 250 MB/s it moves ahead of the view
 * work.
 *
 * Four measurements, because they answer different questions:
 *
 *   ingest scan        the Phase 0 baseline, retained here so the two numbers
 *                      are directly comparable rather than being read across
 *                      two different documents
 *   parse, zero copy   the state machine and the index alone, over bytes the
 *                      caller already has. This is the mmap and Uint8Array path
 *                      and it is what the SIMD decision is about
 *   parse, streamed    the same, plus the copy into the arena, which is what a
 *                      ReadableStream costs
 *   parse + digests    with row hashing on, which is what the diff engine will
 *                      actually run
 */
/* clock_gettime is POSIX, and -std=c11 hides it behind glibc's feature test
 * macros. The benchmark is a host tool, so it asks for POSIX explicitly rather
 * than the build loosening the standard for the library too. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ibha_csvdiff.h"

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 20 columns, roughly 170 bytes per row, a realistic sprinkling of quoted
 * fields. Shape matters: the state machine is branch sensitive and a buffer of
 * one repeated byte would flatter it. */
static size_t fill_csv(uint8_t *buf, size_t cap) {
    static const char *header =
        "acct,dt,name,region,amount,ccy,status,term,note,active,rate,eff,rgn,team,n1,n2,st2,alt,x,y\n";
    static const char *row =
        "ACC-0000123,2026-01-15,\"Smith, John\",NORTH,1250.00,USD,ACTIVE,4,\"note, with comma\","
        "TRUE,0.075,2026-02-01,REGION-7,SALES,12,98.60,PENDING,\"O\"\"Brien\",X,99\n";

    size_t off = strlen(header);
    memcpy(buf, header, off);

    size_t rowlen = strlen(row);
    while (off + rowlen <= cap) {
        memcpy(buf + off, row, rowlen);
        off += rowlen;
    }
    return off;
}

typedef struct {
    const uint8_t *bytes;
    size_t len, pos, stride;
} chunk_reader;

static int64_t chunk_read(void *rc, uint8_t *dst, size_t cap) {
    chunk_reader *r = (chunk_reader *)rc;
    size_t left = r->len - r->pos;
    if (left == 0) return 0;
    size_t n = left < r->stride ? left : r->stride;
    if (n > cap) n = cap;
    memcpy(dst, r->bytes + r->pos, n);
    r->pos += n;
    return (int64_t)n;
}

typedef struct {
    double seconds;
    uint64_t reserved;
    uint32_t rows;
    uint32_t fields;
} result;

static void parse_opts_for(ibha_csvd_parse_opts *o, size_t len, int hash_rows) {
    ibha_csvd_parse_opts_init(o);
    o->header.rows = 1;
    o->header.key_row = 0;
    o->header.required_row = 0;
    o->header.type_row = 0;
    o->header.name_row = 1;
    o->size_hint = (uint64_t)len;
    o->hash_rows = hash_rows;
}

static int run_once(const uint8_t *buf, size_t len, int borrow, int hash_rows, size_t stride,
                    result *out) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parse_opts o;
    parse_opts_for(&o, len, hash_rows);

    double t0 = now_seconds();
    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
    ibha_csvd_status st;
    if (borrow) {
        st = ibha_csvd_parse_borrow(p, buf, len);
        if (st == IBHA_CSVD_OK) st = ibha_csvd_parse_finish(p);
    } else {
        chunk_reader r = {buf, len, 0, stride};
        st = ibha_csvd_parse_stream(ctx, chunk_read, &r, &o, &p);
    }
    double elapsed = now_seconds() - t0;

    if (st != IBHA_CSVD_OK) {
        fprintf(stderr, "bench: %s: %s\n", ibha_csvd_status_name(st), ibha_csvd_ctx_error(ctx));
        ibha_csvd_ctx_free(ctx);
        return 0;
    }

    const ibha_csvd_table *t = ibha_csvd_table_of(p);
    out->seconds = elapsed;
    out->reserved = ibha_csvd_ctx_bytes_reserved(ctx);
    out->rows = t->n_rows;
    out->fields = t->n_fields;
    ibha_csvd_ctx_free(ctx);
    return 1;
}

static void report(const char *label, const result *r, size_t len) {
    double mb = (double)len / (1024.0 * 1024.0);
    printf("  %-26s %8.2f ms  %7.0f MB/s  %8.1f M cells/s  arena %5.2f MB (%.2fx)\n", label,
           r->seconds * 1000.0, mb / r->seconds, (double)r->fields / r->seconds / 1e6,
           (double)r->reserved / (1024.0 * 1024.0), (double)r->reserved / (double)len);
}

int main(int argc, char **argv) {
    size_t mb = 15; /* the stated p90 */
    int repeat = 5;
    size_t stride = 256 * 1024;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mb") == 0 && i + 1 < argc) {
            mb = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stride") == 0 && i + 1 < argc) {
            stride = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: bench_parse [--mb N] [--repeat N] [--stride BYTES] [--file PATH]\n");
            return 2;
        }
    }
    if (mb == 0) mb = 15;
    if (repeat < 1) repeat = 1;
    if (stride == 0) stride = 256 * 1024;

    uint8_t *buf = NULL;
    size_t len = 0;

    if (path) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "bench: cannot open %s\n", path);
            return 2;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (uint8_t *)malloc((size_t)n);
        len = fread(buf, 1, (size_t)n, f);
        fclose(f);
    } else {
        size_t cap = mb * 1024 * 1024;
        buf = (uint8_t *)malloc(cap);
        if (!buf) {
            fprintf(stderr, "bench: cannot allocate %zu MB\n", mb);
            return 2;
        }
        len = fill_csv(buf, cap);
    }

    printf("ibha-csvdiff parser benchmark\n");
    printf("  input       %.2f MB, %s\n", (double)len / (1024.0 * 1024.0),
           path ? path : "20 columns, CSV shaped, in memory");
    printf("  repeats     %d, best reported; stream chunk %zu KB\n\n", repeat, stride / 1024);

    /* The Phase 0 baseline, kept alongside so the comparison is in one place. */
    {
        double best = 0;
        for (int i = 0; i < repeat; i++) {
            ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
            ibha_csvd_buffer_reader r;
            ibha_csvd_buffer_reader_init(&r, buf, len);
            double t0 = now_seconds();
            ibha_csvd_ingest(ctx, ibha_csvd_buffer_read, &r, 0, (uint64_t)len, NULL);
            double e = now_seconds() - t0;
            if (best == 0 || e < best) best = e;
            ibha_csvd_ctx_free(ctx);
        }
        printf("  %-26s %8.2f ms  %7.0f MB/s   (Phase 0 reference: a byte counter, not a parser)\n",
               "ingest scan", best * 1000.0, ((double)len / (1024.0 * 1024.0)) / best);
    }

    struct {
        const char *label;
        int borrow;
        int hash;
    } modes[] = {
        {"parse, zero copy", 1, 0},
        {"parse, streamed", 0, 0},
        {"parse + digests, zero copy", 1, 1},
        {"parse + digests, streamed", 0, 1},
    };

    double parse_only_mbs = 0;
    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        result best = {0, 0, 0, 0};
        for (int i = 0; i < repeat; i++) {
            result r;
            if (!run_once(buf, len, modes[m].borrow, modes[m].hash, stride, &r)) {
                free(buf);
                return 2;
            }
            if (best.seconds == 0 || r.seconds < best.seconds) best = r;
        }
        report(modes[m].label, &best, len);
        if (m == 0) parse_only_mbs = ((double)len / (1024.0 * 1024.0)) / best.seconds;

        if (m == 0) {
            printf("  %-26s %u rows, %u cells, %.1f bytes per cell\n", "", best.rows, best.fields,
                   (double)len / (double)best.fields);
        }
    }

    printf("\n  Phase 1 gate (spec 3.3 and section 11): the scalar parser at or above\n");
    printf("  300 MB/s keeps SIMD in Phase 7 as a batch cost lever. Below 250 MB/s it\n");
    printf("  moves ahead of the view work.\n");
    printf("  measured: %.0f MB/s -> %s\n", parse_only_mbs,
           parse_only_mbs >= 300 ? "SIMD stays in Phase 7"
                                 : (parse_only_mbs >= 250 ? "borderline, re-measure on the target hardware"
                                                          : "move SIMD ahead of the view work"));

    free(buf);
    return 0;
}
