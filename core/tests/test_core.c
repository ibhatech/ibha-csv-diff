/*
 * test_core.c - Phase 0 unit tests: arena, bytebuf, context, error discipline,
 * readers, ingest and limit enforcement.
 */
#include <stdlib.h>

#include "../src/internal.h"
#define TAP_MAIN
#include "suites.h"
#include "tap.h"

/* ------------------------------------------------------------------ arena -- */

static void test_arena(void) {
    ibha_arena a;
    ibha_arena_init(&a);

    void *p1 = ibha_arena_alloc(&a, 1);
    void *p2 = ibha_arena_alloc(&a, 1);
    TAP_OK(p1 && p2 && p1 != p2, "arena hands out distinct pointers");
    TAP_OK(((uintptr_t)p1 % IBHA_ARENA_ALIGN) == 0, "arena result is aligned");
    TAP_OK(((uintptr_t)p2 - (uintptr_t)p1) == IBHA_ARENA_ALIGN,
           "a 1 byte request consumes exactly one alignment unit");

    /* Zeroing is a property of calloc only, so assert it explicitly rather than
     * relying on fresh pages happening to be zero. */
    uint8_t *z = (uint8_t *)ibha_arena_calloc(&a, 512);
    int all_zero = 1;
    for (int i = 0; i < 512; i++) {
        if (z[i] != 0) all_zero = 0;
    }
    TAP_OK(all_zero, "arena_calloc zeroes its result");

    /* Force several block growths and confirm reserved tracks them. */
    uint64_t before = a.reserved;
    int all_ok = 1;
    for (int i = 0; i < 64; i++) {
        if (ibha_arena_alloc(&a, 32 * 1024) == NULL) all_ok = 0;
    }
    TAP_OK(all_ok, "64 repeated 32 KB allocations all succeed");
    TAP_OK(a.reserved > before, "arena grows across blocks");

    /* An allocation larger than the max block size must still be served by a
     * single oversized block rather than failing. */
    void *big = ibha_arena_alloc(&a, IBHA_ARENA_MAX_BLOCK * 2);
    TAP_OK(big != NULL, "allocation larger than the max block size still succeeds");

    ibha_arena_destroy(&a);
    TAP_OK(a.head == NULL && a.reserved == 0, "arena_destroy resets bookkeeping");
}

static void test_bytebuf(void) {
    ibha_arena a;
    ibha_arena_init(&a);

    ibha_bytebuf b = {0};
    const uint8_t src[4] = {'a', 'b', 'c', 'd'};

    /* Append enough times to force at least one reallocation and copy. */
    for (int i = 0; i < 100000; i++) {
        if (!ibha_bytebuf_append(&a, &b, src, 4)) {
            TAP_OK(0, "bytebuf append succeeded");
            break;
        }
    }
    TAP_EQ_U64(b.len, 400000, "bytebuf accumulated the expected length");

    int intact = 1;
    for (size_t i = 0; i < b.len; i++) {
        if (b.bytes[i] != src[i % 4]) {
            intact = 0;
            break;
        }
    }
    TAP_OK(intact, "bytebuf contents survive reallocation");

    ibha_arena_destroy(&a);
}

/* ---------------------------------------------------------------- context -- */

static void test_ctx_and_errors(void) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    TAP_OK(ctx != NULL, "ctx_new succeeds with default limits");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "a fresh context is OK");
    TAP_EQ_STR(ibha_csvd_ctx_error(ctx), "", "a fresh context has an empty error message");

    ibha_err(ctx, IBHA_CSVD_ERR_DUPLICATE_KEY,
             "duplicate key (%s) in uploaded file at rows %llu and %llu", "ACC-10231",
             (uint64_t)4812, (uint64_t)60144);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_DUPLICATE_KEY, "error status recorded");
    TAP_EQ_STR(ibha_csvd_ctx_error(ctx),
               "duplicate key (ACC-10231) in uploaded file at rows 4812 and 60144",
               "formatter handles %s and %llu");

    /* Spec 13.5: first error wins. A downstream symptom must not overwrite the
     * original cause, or the message points at the wrong place. */
    ibha_err(ctx, IBHA_CSVD_ERR_IO, "a later unrelated failure");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_DUPLICATE_KEY,
               "first error wins over a later one");
    TAP_EQ_STR(ibha_csvd_ctx_error(ctx),
               "duplicate key (ACC-10231) in uploaded file at rows 4812 and 60144",
               "first error message is preserved");

    ibha_csvd_ctx_free(ctx);

    /* Every accessor must tolerate NULL rather than trapping, because a binding
     * that failed to construct a context will call them. */
    TAP_EQ_U64(ibha_csvd_ctx_status(NULL), IBHA_CSVD_ERR_INVALID_ARG, "status(NULL) is INVALID_ARG");
    TAP_OK(ibha_csvd_ctx_error(NULL) != NULL, "error(NULL) returns a string, not NULL");
    ibha_csvd_ctx_free(NULL);
    TAP_OK(1, "ctx_free(NULL) does not crash");
}

static void test_status_names(void) {
    TAP_EQ_STR(ibha_csvd_status_name(IBHA_CSVD_OK), "OK", "status name for OK");
    TAP_EQ_STR(ibha_csvd_status_name(IBHA_CSVD_ERR_TOO_LARGE), "TOO_LARGE",
               "status name for TOO_LARGE");
    TAP_EQ_STR(ibha_csvd_status_name((ibha_csvd_status)-999), "UNKNOWN",
               "unknown status codes are named, not left NULL");

    int major = -1, minor = -1, patch = -1;
    ibha_csvd_version(&major, &minor, &patch);
    TAP_EQ_U64(major, IBHA_CSVD_VERSION_MAJOR, "version major matches the header");
}

/* ----------------------------------------------------------------- ingest -- */

static void test_ingest_buffer(void) {
    static const char csv[] = "id,name\n1,\"Smith, J\"\n2,\"O\"\"Brien\"\n";
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);

    ibha_csvd_buffer_reader r;
    ibha_csvd_buffer_reader_init(&r, csv, sizeof(csv) - 1);

    ibha_csvd_ingest_stats st;
    ibha_csvd_status s = ibha_csvd_ingest(ctx, ibha_csvd_buffer_read, &r, 1, 0, &st);

    TAP_EQ_U64(s, IBHA_CSVD_OK, "ingest of a small buffer succeeds");
    TAP_EQ_U64(st.bytes, sizeof(csv) - 1, "ingest counted every byte");
    TAP_EQ_U64(st.line_breaks, 3, "ingest counted line breaks");
    TAP_EQ_U64(st.quotes, 6, "ingest counted quote characters");

    size_t len = 0;
    const uint8_t *bytes = ibha_csvd_ingest_bytes(ctx, &len);
    TAP_EQ_U64(len, sizeof(csv) - 1, "retained byte count matches");
    TAP_OK(bytes && memcmp(bytes, csv, len) == 0, "retained bytes are byte identical to input");

    ibha_csvd_ctx_free(ctx);
}

static void test_ingest_no_retain(void) {
    static const char csv[] = "a,b\n1,2\n";
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);

    ibha_csvd_buffer_reader r;
    ibha_csvd_buffer_reader_init(&r, csv, sizeof(csv) - 1);

    ibha_csvd_ingest_stats st;
    ibha_csvd_ingest(ctx, ibha_csvd_buffer_read, &r, 0, 0, &st);

    size_t len = 999;
    TAP_OK(ibha_csvd_ingest_bytes(ctx, &len) == NULL, "retain=0 retains nothing");
    TAP_EQ_U64(len, 0, "retain=0 reports zero length");

    ibha_csvd_ctx_free(ctx);
}

/* A reader that reports a hard failure partway through, to prove the error path
 * surfaces IO_ERROR rather than silently truncating the input. Silent truncation
 * would produce a plausible looking diff from half a file, which is the worst
 * possible failure mode for this library. */
static int64_t failing_read(void *read_ctx, uint8_t *dst, size_t cap) {
    int *calls = (int *)read_ctx;
    if (*calls > 0) {
        (*calls)--;
        for (size_t i = 0; i < cap; i++) dst[i] = 'x';
        return (int64_t)cap;
    }
    return -1;
}

static void test_ingest_io_error(void) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    int calls = 2;
    ibha_csvd_status s = ibha_csvd_ingest(ctx, failing_read, &calls, 0, 0, NULL);

    TAP_EQ_U64(s, IBHA_CSVD_ERR_IO, "a failing source surfaces IO_ERROR");
    TAP_OK(ibha_csvd_ctx_error(ctx)[0] != '\0', "IO_ERROR carries a message");

    ibha_csvd_ctx_free(ctx);
}

static void test_ingest_limit(void) {
    /* A 1 MB limit against a source that keeps producing. The limit must trip on
     * arrival, not after buffering, per spec 13.4. */
    ibha_csvd_limits lim;
    ibha_csvd_limits_init(&lim);
    lim.max_bytes = 1024 * 1024;

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(&lim);

    size_t big = 4u * 1024u * 1024u;
    uint8_t *blob = (uint8_t *)malloc(big);
    memset(blob, 'a', big);

    ibha_csvd_buffer_reader r;
    ibha_csvd_buffer_reader_init(&r, blob, big);

    ibha_csvd_status s = ibha_csvd_ingest(ctx, ibha_csvd_buffer_read, &r, 1, 0, NULL);
    TAP_EQ_U64(s, IBHA_CSVD_ERR_TOO_LARGE, "exceeding max_bytes yields TOO_LARGE");
    TAP_EQ_STR(ibha_csvd_ctx_error(ctx),
               "File too large. Maximum allowed is 1 MB, received at least 2 MB.",
               "TOO_LARGE message states both the limit and what was seen");

    /* The whole 4 MB must not have been retained before failing. */
    TAP_OK(ibha_csvd_ctx_bytes_reserved(ctx) < big,
           "limit trips before the oversized input is fully buffered");

    free(blob);
    ibha_csvd_ctx_free(ctx);

    /* Exactly at the limit is allowed; only exceeding it fails. */
    ibha_csvd_limits_init(&lim);
    lim.max_bytes = 8;
    ctx = ibha_csvd_ctx_new(&lim);
    ibha_csvd_buffer_reader_init(&r, "12345678", 8);
    TAP_EQ_U64(ibha_csvd_ingest(ctx, ibha_csvd_buffer_read, &r, 0, 0, NULL), IBHA_CSVD_OK,
               "a source exactly at the limit is accepted");
    ibha_csvd_ctx_free(ctx);
}

static void test_ingest_empty(void) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_buffer_reader r;
    ibha_csvd_buffer_reader_init(&r, "", 0);

    ibha_csvd_ingest_stats st;
    TAP_EQ_U64(ibha_csvd_ingest(ctx, ibha_csvd_buffer_read, &r, 1, 0, &st), IBHA_CSVD_OK,
               "an empty source is not an error");
    TAP_EQ_U64(st.bytes, 0, "empty source reports zero bytes");
    ibha_csvd_ctx_free(ctx);
}

/* A reader that hands back an awkward, prime number of bytes per call, so that
 * chunk boundaries land in the middle of interesting byte sequences. */
typedef struct {
    const uint8_t *bytes;
    size_t len, pos, stride;
} dribble_reader;

static int64_t dribble_read(void *read_ctx, uint8_t *dst, size_t cap) {
    dribble_reader *r = (dribble_reader *)read_ctx;
    size_t left = r->len - r->pos;
    if (left == 0) return 0;
    size_t n = left < r->stride ? left : r->stride;
    if (n > cap) n = cap;
    memcpy(dst, r->bytes + r->pos, n);
    r->pos += n;
    return (int64_t)n;
}

static void test_ingest_chunk_boundaries(void) {
    /*
     * The reader may return any chunk size. Ingest must produce identical tallies
     * regardless. This is the property the resumable parser will depend on
     * (spec 2.5 point 2), so it is locked down at the ingest layer before the
     * state machine arrives, where a violation would be far harder to localize.
     */
    size_t n = 300000;
    uint8_t *blob = (uint8_t *)malloc(n);
    for (size_t i = 0; i < n; i++) blob[i] = (i % 37 == 0) ? '\n' : (i % 53 == 0 ? '"' : 'z');

    ibha_csvd_ingest_stats whole;
    ibha_csvd_ctx *c1 = ibha_csvd_ctx_new(NULL);
    ibha_csvd_buffer_reader r1;
    ibha_csvd_buffer_reader_init(&r1, blob, n);
    ibha_csvd_ingest(c1, ibha_csvd_buffer_read, &r1, 0, 0, &whole);
    ibha_csvd_ctx_free(c1);
    TAP_EQ_U64(whole.bytes, n, "whole buffer ingest saw every byte");

    const size_t strides[] = {1, 7, 4093, 65537};
    for (size_t k = 0; k < sizeof(strides) / sizeof(strides[0]); k++) {
        dribble_reader dr = {blob, n, 0, strides[k]};
        ibha_csvd_ingest_stats got;
        ibha_csvd_ctx *c = ibha_csvd_ctx_new(NULL);
        ibha_csvd_ingest(c, dribble_read, &dr, 0, 0, &got);
        ibha_csvd_ctx_free(c);

        int same = got.bytes == whole.bytes && got.line_breaks == whole.line_breaks &&
                   got.quotes == whole.quotes;
        TAP_OK(same, "ingest tallies are independent of reader chunk size");
    }

    free(blob);
}

static void test_size_hint(void) {
    /*
     * Retaining N bytes without a hint costs roughly 4x N, because the buffer
     * doubles and the arena does not reclaim the abandoned copies. With a hint it
     * should be close to 1x. This ratio decides how many concurrent diffs fit on
     * a batch worker (spec 2.6.5), so it is asserted rather than left to drift.
     */
    size_t n = 4u * 1024u * 1024u;
    uint8_t *blob = (uint8_t *)malloc(n);
    memset(blob, 'q', n);

    uint64_t without = 0, with_hint = 0;

    ibha_csvd_ctx *c = ibha_csvd_ctx_new(NULL);
    ibha_csvd_buffer_reader r;
    ibha_csvd_buffer_reader_init(&r, blob, n);
    ibha_csvd_ingest(c, ibha_csvd_buffer_read, &r, 1, 0, NULL);
    without = ibha_csvd_ctx_bytes_reserved(c);
    ibha_csvd_ctx_free(c);

    c = ibha_csvd_ctx_new(NULL);
    ibha_csvd_buffer_reader_init(&r, blob, n);
    ibha_csvd_ingest(c, ibha_csvd_buffer_read, &r, 1, (uint64_t)n, NULL);
    with_hint = ibha_csvd_ctx_bytes_reserved(c);

    size_t got_len = 0;
    const uint8_t *got = ibha_csvd_ingest_bytes(c, &got_len);
    TAP_EQ_U64(got_len, n, "hinted ingest retained the full input");
    TAP_OK(got && got[0] == 'q' && got[n - 1] == 'q', "hinted ingest retained correct bytes");
    ibha_csvd_ctx_free(c);

    TAP_OK(with_hint < without, "a size hint reduces arena reservation");
    TAP_OK(with_hint < (uint64_t)n * 3 / 2,
           "hinted reservation stays under 1.5x the input size");

    /* A hint that overruns the source is a performance mistake, not a
     * correctness one: the result must still be exactly what arrived. */
    ibha_csvd_limits lim;
    ibha_csvd_limits_init(&lim);
    c = ibha_csvd_ctx_new(&lim);
    ibha_csvd_buffer_reader_init(&r, blob, 1000);
    ibha_csvd_ingest_stats st;
    ibha_csvd_ingest(c, ibha_csvd_buffer_read, &r, 1, 999999, &st);
    TAP_EQ_U64(st.bytes, 1000, "an over-large hint does not change the byte count");
    ibha_csvd_ingest_bytes(c, &got_len);
    TAP_EQ_U64(got_len, 1000, "an over-large hint does not change retained length");
    ibha_csvd_ctx_free(c);

    /* A hint already over the limit must be rejected before any read happens. */
    ibha_csvd_limits_init(&lim);
    lim.max_bytes = 1024 * 1024;
    c = ibha_csvd_ctx_new(&lim);
    ibha_csvd_buffer_reader_init(&r, blob, n);
    TAP_EQ_U64(ibha_csvd_ingest(c, ibha_csvd_buffer_read, &r, 1, (uint64_t)n, NULL),
               IBHA_CSVD_ERR_TOO_LARGE, "an oversized hint fails before reading");
    TAP_EQ_U64(r.pos, 0, "no bytes were read when the hint already exceeded the limit");
    ibha_csvd_ctx_free(c);

    free(blob);
}

int main(int argc, char **argv) {
    const char *fixtures = argc > 1 ? argv[1] : "fixtures/generated";

    test_arena();
    test_bytebuf();
    test_ctx_and_errors();
    test_status_names();
    test_ingest_buffer();
    test_ingest_no_retain();
    test_ingest_io_error();
    test_ingest_limit();
    test_ingest_empty();
    test_ingest_chunk_boundaries();
    test_size_hint();

    ibha_test_hash();
    ibha_test_parse();
    ibha_test_schema();
    ibha_test_normalize();
    ibha_test_match();
    ibha_test_diff();
    ibha_test_validate();
    ibha_test_columns();
    ibha_test_segment();
    ibha_test_emit(fixtures);
    ibha_test_property(fixtures);
    TAP_DONE();
}
