/*
 * test_property.c - the two properties that hold for every input.
 *
 * 1. **Chunking is invisible.** Parsing a file whole and parsing it in chunks of
 *    any size must produce byte identical indexes, and identical errors when it
 *    fails. This is the property spec 2.5 makes structural, and it is asserted
 *    across every fixture at eight fixed strides plus a randomized one, so a
 *    boundary lands inside a quoted field, an escape pair, a CRLF and the BOM
 *    many thousands of times over.
 *
 * 2. **Quoting is invisible.** Re-emitting a file under a different quoting
 *    policy and parsing it back must yield the same logical values and the same
 *    row digests. That is spec 5.2's requirement stated as a test: a spreadsheet
 *    round trip that requotes every cell must not report a single change.
 *
 * Both are run over the generated fixture corpus, which is where the BOM, the
 * mixed line endings, the multiline cells, the escaped quotes, the non UTF-8
 * bytes and the XSS payload live.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/internal.h"
#include "suites.h"
#include "tap.h"

/* Every generated edge case, plus the 1 MB pair, which is the only fixture large
 * enough to exercise the index growth and the size hint extrapolation. */
static const char *const k_fixtures[] = {
    "empty.csv",       "header_only.csv",     "no_trailing_newline.csv", "crlf.csv",
    "mixed_endings.csv", "bom.csv",           "multiline_quoted.csv",    "escaped_quotes.csv",
    "ragged.csv",      "names_only_header.csv", "xss.csv",              "latin1.csv",
    "tiny_source.csv", "tiny_target.csv",
};

/* ------------------------------------------------------------------ tools -- */

static uint8_t *read_file(const char *dir, const char *name, size_t *out_len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    *out_len = got;
    return buf;
}

int ibha_tables_identical(const ibha_csvd_table *a, const ibha_csvd_table *b) {
    if (a->n_rows != b->n_rows || a->n_fields != b->n_fields || a->n_columns != b->n_columns) {
        return 0;
    }
    /* Byte counts are deliberately not compared. A parse that fails stops
     * consuming input, so a streamed parse of a malformed file has seen fewer
     * bytes than a one shot parse of the same file even though both produced the
     * same index and the same error. What has to match is the index. */
    for (uint32_t i = 0; i < a->n_fields; i++) {
        if (a->field_off[i] != b->field_off[i] || a->field_len[i] != b->field_len[i] ||
            a->field_flags[i] != b->field_flags[i]) {
            return 0;
        }
    }
    for (uint32_t r = 0; r <= a->n_rows; r++) {
        if (a->row_first_field[r] != b->row_first_field[r]) return 0;
    }
    for (uint32_t r = 0; r < a->n_rows; r++) {
        if (a->row_key_hash[r] != b->row_key_hash[r] ||
            a->row_full_hash[r] != b->row_full_hash[r] ||
            a->row_raw_hash[r] != b->row_raw_hash[r]) {
            return 0;
        }
    }
    return 1;
}

/* Property tests run with no header model, so that every row is data and
 * therefore digested: the header rows are then covered by the same assertions as
 * the data rows rather than being skipped. */
static void property_opts(ibha_csvd_parse_opts *o, uint64_t size_hint) {
    ibha_csvd_parse_opts_init(o);
    o->header.rows = 0;
    o->header.key_row = 0;
    o->header.required_row = 0;
    o->header.type_row = 0;
    o->header.name_row = 0;
    o->size_hint = size_hint;
}

static uint32_t xorshift(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* ------------------------------------------- property 1: chunking is invisible */

static int chunked_matches(const uint8_t *body, size_t len, const ibha_csvd_table *want,
                           ibha_csvd_status want_status, size_t stride, uint32_t *rng) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parse_opts o;
    property_opts(&o, len);

    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
    for (size_t off = 0; off < len;) {
        /* stride 0 means "randomize", which is what finds the boundary nobody
         * thought to enumerate. */
        size_t n = stride ? stride : (size_t)(xorshift(rng) % 97u) + 1u;
        if (n > len - off) n = len - off;
        if (ibha_csvd_parse_chunk(p, body + off, n) != IBHA_CSVD_OK) break;
        off += n;
    }
    ibha_csvd_parse_finish(p);

    int ok = ibha_csvd_ctx_status(ctx) == want_status &&
             ibha_tables_identical(ibha_csvd_table_of(p), want);
    ibha_csvd_ctx_free(ctx);
    return ok;
}

static void test_chunk_invariance(const char *dir) {
    static const size_t strides[] = {0, 1, 2, 3, 7, 13, 64, 997, 65536};
    uint32_t rng = 0x2545F491u;
    int failures = 0;
    int files = 0;

    for (size_t i = 0; i < sizeof(k_fixtures) / sizeof(k_fixtures[0]); i++) {
        size_t len = 0;
        uint8_t *body = read_file(dir, k_fixtures[i], &len);
        if (!body) {
            TAP_OK(0, k_fixtures[i]);
            printf("  missing fixture, run: make fixtures\n");
            failures++;
            continue;
        }
        files++;

        /* The reference: the whole file in one go, borrowed rather than copied,
         * so the zero copy path is on the reference side of every comparison. */
        ibha_csvd_ctx *ref_ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parse_opts o;
        property_opts(&o, len);
        ibha_csvd_parser *ref = ibha_csvd_parse_begin(ref_ctx, &o);
        ibha_csvd_parse_borrow(ref, body, len);
        ibha_csvd_parse_finish(ref);

        ibha_csvd_status want = ibha_csvd_ctx_status(ref_ctx);
        for (size_t s = 0; s < sizeof(strides) / sizeof(strides[0]); s++) {
            for (int rep = 0; rep < (strides[s] == 0 ? 8 : 1); rep++) {
                if (!chunked_matches(body, len, ibha_csvd_table_of(ref), want, strides[s], &rng)) {
                    printf("  mismatch in %s at stride %zu\n", k_fixtures[i], strides[s]);
                    failures++;
                }
            }
        }

        ibha_csvd_ctx_free(ref_ctx);
        free(body);
    }

    TAP_EQ_U64(failures, 0, "every fixture parses identically at every chunk size");
    TAP_OK(files == (int)(sizeof(k_fixtures) / sizeof(k_fixtures[0])),
           "every fixture in the corpus was found and read");
}

/* -------------------------------------------- property 2: quoting is invisible */

typedef struct {
    uint8_t *bytes;
    size_t len, cap;
} outbuf;

static void out_push(outbuf *b, const uint8_t *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->bytes = (uint8_t *)realloc(b->bytes, b->cap);
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}

static void out_byte(outbuf *b, uint8_t c) { out_push(b, &c, 1); }

enum { Q_MINIMAL = 0, Q_ALL, Q_NON_NUMERIC };

static int needs_quoting(const uint8_t *v, uint32_t n, int policy) {
    if (policy == Q_ALL) return 1;
    for (uint32_t i = 0; i < n; i++) {
        if (v[i] == ',' || v[i] == '"' || v[i] == '\r' || v[i] == '\n') return 1;
    }
    if (policy == Q_NON_NUMERIC) {
        if (n == 0) return 0;
        for (uint32_t i = 0; i < n; i++) {
            if ((v[i] < '0' || v[i] > '9') && v[i] != '.' && v[i] != '-' && v[i] != '+') return 1;
        }
    }
    return 0;
}

/* Writes the table back out as CSV under one quoting policy, working from the
 * logical values so the result is a genuine requoting rather than a byte copy. */
static void reemit(const ibha_csvd_table *t, int policy, outbuf *out) {
    uint8_t *cell = NULL;
    size_t cell_cap = 0;

    for (uint32_t r = 0; r < t->n_rows; r++) {
        uint32_t first = t->row_first_field[r];
        uint32_t count = t->row_first_field[r + 1] - first;

        for (uint32_t c = 0; c < count; c++) {
            uint32_t f = first + c;
            uint32_t need = ibha_csvd_field_logical_len(t, f);
            if ((size_t)need + 1 > cell_cap) {
                cell_cap = (size_t)need * 2 + 64;
                cell = (uint8_t *)realloc(cell, cell_cap);
            }
            ibha_csvd_field_copy(t, f, cell, cell_cap);

            if (c) out_byte(out, ',');
            if (needs_quoting(cell, need, policy)) {
                out_byte(out, '"');
                for (uint32_t i = 0; i < need; i++) {
                    if (cell[i] == '"') out_byte(out, '"');
                    out_byte(out, cell[i]);
                }
                out_byte(out, '"');
            } else {
                out_push(out, cell, need);
            }
        }
        out_byte(out, '\n');
    }
    free(cell);
}

/* The Phase 1 form of "the diff is empty": same shape, every cell logically
 * equal, and identical row digests. */
static int tables_equal_logically(const ibha_csvd_table *a, const ibha_csvd_table *b) {
    if (a->n_rows != b->n_rows || a->n_fields != b->n_fields) return 0;
    for (uint32_t r = 0; r < a->n_rows; r++) {
        if (a->row_first_field[r] != b->row_first_field[r]) return 0;
        if (a->row_key_hash[r] != b->row_key_hash[r]) return 0;
        if (a->row_full_hash[r] != b->row_full_hash[r]) return 0;
    }
    for (uint32_t f = 0; f < a->n_fields; f++) {
        if (ibha_csvd_field_cmp(a, f, b, f) != 0) return 0;
        if (ibha_csvd_field_hash(a, f) != ibha_csvd_field_hash(b, f)) return 0;
    }
    return 1;
}

static void test_requoting_invariance(const char *dir) {
    static const char *const policy_name[] = {"minimal", "quote every field",
                                              "quote every non numeric field"};
    int failures = 0;
    int compared = 0;

    for (size_t i = 0; i < sizeof(k_fixtures) / sizeof(k_fixtures[0]); i++) {
        size_t len = 0;
        uint8_t *body = read_file(dir, k_fixtures[i], &len);
        if (!body) continue;

        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parse_opts o;
        property_opts(&o, len);
        ibha_csvd_parser *orig = ibha_csvd_parse_begin(ctx, &o);
        ibha_csvd_parse_borrow(orig, body, len);
        ibha_csvd_parse_finish(orig);

        /* A fixture that is deliberately malformed, like the ragged one, has
         * nothing to re-emit. */
        if (ibha_csvd_ctx_status(ctx) != IBHA_CSVD_OK) {
            ibha_csvd_ctx_free(ctx);
            free(body);
            continue;
        }

        for (int policy = 0; policy < 3; policy++) {
            outbuf out = {NULL, 0, 0};
            reemit(ibha_csvd_table_of(orig), policy, &out);

            ibha_csvd_ctx *c2 = ibha_csvd_ctx_new(NULL);
            ibha_csvd_parse_opts o2;
            property_opts(&o2, out.len);
            ibha_csvd_parser *again = ibha_csvd_parse_begin(c2, &o2);
            ibha_csvd_parse_borrow(again, out.bytes, out.len);
            ibha_csvd_parse_finish(again);

            if (ibha_csvd_ctx_status(c2) != IBHA_CSVD_OK ||
                !tables_equal_logically(ibha_csvd_table_of(orig), ibha_csvd_table_of(again))) {
                printf("  %s differs after re-emitting with policy: %s\n", k_fixtures[i],
                       policy_name[policy]);
                failures++;
            }
            compared++;

            ibha_csvd_ctx_free(c2);
            free(out.bytes);
        }

        ibha_csvd_ctx_free(ctx);
        free(body);
    }

    TAP_EQ_U64(failures, 0, "re-emitting under three quoting policies changes nothing");
    TAP_OK(compared >= 30, "the requoting property covered the whole parseable corpus");
}

/* ---------------------------------------------------------------- memory ---- */

static void test_index_memory(const char *dir) {
    /*
     * Phase 0 learned that a growable structure in an arena that never reclaims
     * costs about 4x the final size without a size hint. The index arrays have
     * the same shape, so the same ratio is asserted here rather than left to
     * drift, because spec 2.6.5 makes per worker memory the thing that sets how
     * many diffs a batch worker can run at once.
     */
    size_t len = 0;
    uint8_t *body = read_file(dir, "tiny_source.csv", &len);
    if (!body || len == 0) {
        TAP_OK(0, "tiny_source.csv is available for the memory assertion");
        free(body);
        return;
    }

    uint64_t with_hint = 0, without = 0;
    uint32_t rows = 0;

    for (int hinted = 0; hinted < 2; hinted++) {
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parse_opts o;
        property_opts(&o, hinted ? len : 0);

        ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
        for (size_t off = 0; off < len;) {
            size_t n = len - off < 65536 ? len - off : 65536;
            ibha_csvd_parse_chunk(p, body + off, n);
            off += n;
        }
        ibha_csvd_parse_finish(p);
        rows = ibha_csvd_table_of(p)->n_rows;

        if (hinted) {
            with_hint = ibha_csvd_ctx_bytes_reserved(ctx);
        } else {
            without = ibha_csvd_ctx_bytes_reserved(ctx);
        }
        ibha_csvd_ctx_free(ctx);
    }

    TAP_OK(rows > 1000, "the memory fixture is large enough to force index growth");
    TAP_OK(with_hint < without, "a size hint reduces the parser's arena reservation");

    /* Bytes plus 9 per cell plus 20 per row is about 2.4x this fixture's shape,
     * so 3x is the ceiling that catches a doubling regression without being
     * sensitive to the exact column count. */
    TAP_OK(with_hint < (uint64_t)len * 3,
           "a hinted parse reserves under 3x the input, index included");
    free(body);
}

/* --------------------------- property 3: the diff recovers the edit script -- */
/*
 * The headline property of spec section 10. The fixture generator applies a
 * known edit script to a 15 MB file and writes the script out beside it; the
 * diff has to produce exactly that script back, with nothing extra and nothing
 * missing.
 *
 * That is a much stronger statement than any hand written case. It exercises
 * 146,000 rows through the real bucket map, the real longest increasing
 * subsequence, anchored deletion placement and the declared type comparators at
 * once, and it fails loudly if any of them invents a change, loses one, or
 * reports one row as moved when the answer is a different row.
 *
 * The script is read from the .tsv rather than the .json beside it: the shape is
 * fixed, and a JSON parser inside the test suite would be a second thing to keep
 * correct for no benefit. Both sides emit the same tuples, both sort them, and
 * the comparison is element by element.
 */
#define EDIT_LINE_CAP 160

typedef struct {
    char (*line)[EDIT_LINE_CAP];
    size_t len, cap;
} editlist;

static void edit_push(editlist *e, const char *s) {
    if (e->len == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 256;
        e->line = (char (*)[EDIT_LINE_CAP])realloc(e->line, e->cap * EDIT_LINE_CAP);
    }
    snprintf(e->line[e->len], EDIT_LINE_CAP, "%s", s);
    e->len++;
}

static int edit_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void edit_sort(editlist *e) { qsort(e->line, e->len, EDIT_LINE_CAP, edit_cmp); }

/* The logical value of a cell, truncated to what an edit line can carry. */
static void cell_text(const ibha_csvd_table *t, uint32_t f, char *dst, size_t cap) {
    uint8_t buf[40];
    uint32_t n = ibha_csvd_field_copy(t, f, buf, sizeof(buf));
    if (n > sizeof(buf)) n = sizeof(buf);
    size_t w = 0;
    for (uint32_t i = 0; i < n && w + 1 < cap; i++) dst[w++] = (char)buf[i];
    dst[w] = '\0';
}

/* "<key column values, tab separated>" for a row, which is how the generator
 * identifies a row too: position is exactly what the edits change. */
static void key_text(const ibha_csvd_table *t, const ibha_csvd_schema *s, uint32_t row, char *dst,
                     size_t cap) {
    size_t w = 0;
    dst[0] = '\0';
    for (uint32_t c = 0; c < s->n_columns; c++) {
        if (!(s->col_flags[c] & IBHA_CSVD_COL_KEY)) continue;
        char v[48];
        cell_text(t, t->row_first_field[row] + c, v, sizeof(v));
        int n = snprintf(dst + w, cap - w, "%s%s", w ? "\t" : "", v);
        if (n < 0 || (size_t)n >= cap - w) return;
        w += (size_t)n;
    }
}

static void collect_diff_edits(ibha_csvd_diff *d, const ibha_csvd_table *src,
                               const ibha_csvd_table *tgt, const ibha_csvd_schema *s,
                               editlist *out) {
    ibha_csvd_cursor *cur = ibha_csvd_cursor_open(d);
    char key[64], line[EDIT_LINE_CAP], name[32];

    while (cur && ibha_csvd_cursor_next(cur) == 1) {
        const ibha_csvd_row *r = ibha_csvd_cursor_row(cur);
        const ibha_csvd_table *side = r->kind == IBHA_CSVD_ROW_DELETED ? src : tgt;
        uint32_t row = r->kind == IBHA_CSVD_ROW_DELETED ? r->source_row : r->target_row;
        key_text(side, s, row, key, sizeof(key));

        if (r->kind == IBHA_CSVD_ROW_ADDED || r->kind == IBHA_CSVD_ROW_DELETED) {
            snprintf(line, sizeof(line), "%c\t%s\t",
                     r->kind == IBHA_CSVD_ROW_ADDED ? 'A' : 'D', key);
            edit_push(out, line);
        } else if (r->kind == IBHA_CSVD_ROW_MODIFIED) {
            for (uint32_t c = 0; c < r->n_columns; c++) {
                if (!(r->cell_flags[c] & IBHA_CSVD_CELL_CHANGED)) continue;
                cell_text(tgt, tgt->row_first_field[s->name_row] + c, name, sizeof(name));
                snprintf(line, sizeof(line), "M\t%s\t%s", key, name);
                edit_push(out, line);
            }
        }
        /* Moved is a flag rather than a kind, so it is emitted alongside
         * whatever the row otherwise was. */
        if (r->moved) {
            snprintf(line, sizeof(line), "V\t%s\t", key);
            edit_push(out, line);
        }
    }
}

static void test_edit_script_recovery(const char *dir) {
    size_t slen = 0, tlen = 0, elen = 0;
    uint8_t *sbytes = read_file(dir, "p90_source.csv", &slen);
    uint8_t *tbytes = read_file(dir, "p90_target.csv", &tlen);
    uint8_t *ebytes = read_file(dir, "p90_edits.tsv", &elen);

    if (!sbytes || !tbytes || !ebytes || slen == 0 || tlen == 0 || elen == 0) {
        TAP_OK(0, "the p90 pair and its edit script are available");
        printf("  missing fixture, run: make fixtures\n");
        free(sbytes);
        free(tbytes);
        free(ebytes);
        return;
    }

    editlist want = {NULL, 0, 0};
    for (size_t i = 0, start = 0; i <= elen; i++) {
        if (i != elen && ebytes[i] != '\n') continue;
        if (i > start) {
            char tmp[EDIT_LINE_CAP];
            size_t n = i - start < sizeof(tmp) - 1 ? i - start : sizeof(tmp) - 1;
            memcpy(tmp, ebytes + start, n);
            tmp[n] = '\0';
            edit_push(&want, tmp);
        }
        start = i + 1;
    }

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parse_opts so;
    ibha_csvd_parse_opts_init(&so);
    so.size_hint = slen;
    ibha_csvd_parser *sp = ibha_csvd_parse_begin(ctx, &so);
    ibha_csvd_parse_borrow(sp, sbytes, slen);
    ibha_csvd_parse_finish(sp);

    ibha_csvd_parse_opts to;
    ibha_csvd_parse_opts_init(&to);
    to.size_hint = tlen;
    to.header.rows = IBHA_CSVD_HEADER_AUTO;
    to.expect_table = ibha_csvd_table_of(sp);
    to.expect_schema = ibha_csvd_schema_of(sp);
    ibha_csvd_parser *tp = ibha_csvd_parse_begin(ctx, &to);
    ibha_csvd_parse_borrow(tp, tbytes, tlen);
    ibha_csvd_parse_finish(tp);

    ibha_csvd_diff *d =
        ibha_csvd_diff_run(ctx, ibha_csvd_table_of(sp), ibha_csvd_schema_of(sp),
                           ibha_csvd_table_of(tp), ibha_csvd_schema_of(tp), NULL);
    TAP_OK(d != NULL, "the 15 MB pair diffs");
    if (!d) {
        printf("  # %s: %s\n", ibha_csvd_status_name(ibha_csvd_ctx_status(ctx)),
               ibha_csvd_ctx_error(ctx));
        ibha_csvd_ctx_free(ctx);
        free(sbytes);
        free(tbytes);
        free(ebytes);
        free(want.line);
        return;
    }

    editlist got = {NULL, 0, 0};
    collect_diff_edits(d, ibha_csvd_table_of(sp), ibha_csvd_table_of(tp), ibha_csvd_schema_of(sp),
                       &got);

    edit_sort(&want);
    edit_sort(&got);

    int same = want.len == got.len;
    size_t shown = 0;
    for (size_t i = 0; i < want.len && i < got.len; i++) {
        if (strcmp(want.line[i], got.line[i]) == 0) continue;
        same = 0;
        if (shown++ < 5) printf("  # want \"%s\" got \"%s\"\n", want.line[i], got.line[i]);
    }
    /* Guards against the comparison passing because both sides are empty, which
     * a broken fixture or a broken cursor would both produce. */
    TAP_OK(want.len > 1000, "the edit script is substantial enough to be worth recovering");
    TAP_EQ_U64(got.len, want.len, "the diff finds exactly as many edits as were applied");
    TAP_OK(same, "and recovers the generator's edit script exactly");

    /* One row was dragged to the front of a 146,000 row file. A move detector
     * without the longest increasing subsequence would report every row after it
     * as moved, which is the failure spec 6.2 exists to prevent. */
    TAP_EQ_U64(ibha_csvd_diff_stats_of(d)->rows_moved, 1,
               "and reports exactly one moved row, not the whole file");

    ibha_csvd_ctx_free(ctx);
    free(sbytes);
    free(tbytes);
    free(ebytes);
    free(want.line);
    free(got.line);
}

void ibha_test_property(const char *fixture_dir) {
    test_chunk_invariance(fixture_dir);
    test_requoting_invariance(fixture_dir);
    test_index_memory(fixture_dir);
    test_edit_script_recovery(fixture_dir);
}
