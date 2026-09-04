/*
 * fuzz_diff.c - libFuzzer entry point for the matcher and the cursor.
 *
 * The parser is fuzzed because untrusted bytes drive its control flow. The
 * matcher is fuzzed for a different reason: it is built out of open addressing
 * probe loops and a longest increasing subsequence, and the failure mode of a
 * wrong bound in either is not a crash but a hang or a silent misreport. A probe
 * loop that never meets an empty slot spins forever; an LIS backtrack with a
 * wrong terminator walks off its array.
 *
 * So this target asserts the accounting rather than just running the code:
 *
 *   - every target row appears exactly once in the report
 *   - every source row appears exactly once, either matched or deleted
 *   - the report yields exactly report_rows rows
 *   - no source row is claimed by two target rows
 *
 * Those four together mean the report is a permutation of the two inputs, which
 * is the property a diff cannot be allowed to break however malformed its input.
 *
 *   make fuzz
 *   build/fuzz_diff -max_total_time=60 corpus/
 */
#include <stddef.h>
#include <stdint.h>

#include "../src/internal.h"

/* A fixed keyed header, so that the keyed path of spec 6.1 is reachable at all.
 * Random bytes will not produce a KEY marker row on their own, and leaving the
 * keyed matcher unfuzzed would miss the half of the code where duplicate
 * detection and key verification live. */
static const char k_keyed_header[] =
    "KEY,,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\n";

static void check_report(ibha_csvd_diff *d, const ibha_csvd_table *src,
                         const ibha_csvd_schema *ss, const ibha_csvd_table *tgt,
                         const ibha_csvd_schema *ts) {
    ibha_csvd_cursor *cur = ibha_csvd_cursor_open(d);
    const ibha_csvd_diff_stats *st = ibha_csvd_diff_stats_of(d);
    if (!cur) return;

    uint32_t n_src = src->n_rows > ss->first_data_row ? src->n_rows - ss->first_data_row : 0;
    uint32_t n_tgt = tgt->n_rows > ts->first_data_row ? tgt->n_rows - ts->first_data_row : 0;

    uint32_t seen = 0;
    uint32_t last_target = 0;
    int have_target = 0;

    while (ibha_csvd_cursor_next(cur) == 1) {
        const ibha_csvd_row *r = ibha_csvd_cursor_row(cur);
        if (!r) __builtin_trap();
        seen++;
        if (seen > n_src + n_tgt) __builtin_trap(); /* the walk is not terminating */

        if (r->kind == IBHA_CSVD_ROW_DELETED) {
            if (r->target_row != IBHA_CSVD_NO_ROW) __builtin_trap();
            if (r->source_row < ss->first_data_row || r->source_row >= src->n_rows) {
                __builtin_trap();
            }
            continue;
        }
        if (r->target_row < ts->first_data_row || r->target_row >= tgt->n_rows) __builtin_trap();
        /* Target rows are emitted in target order and each exactly once, which is
         * spec 6.5's whole statement about where non deleted rows go. */
        if (have_target && r->target_row != last_target + 1) __builtin_trap();
        last_target = r->target_row;
        have_target = 1;

        if (r->kind == IBHA_CSVD_ROW_ADDED) {
            if (r->source_row != IBHA_CSVD_NO_ROW) __builtin_trap();
        } else if (r->source_row < ss->first_data_row || r->source_row >= src->n_rows) {
            __builtin_trap();
        }
    }

    if (seen != st->report_rows) __builtin_trap();
    if (st->rows_unchanged + st->rows_modified + st->rows_added != n_tgt) __builtin_trap();
    if (st->rows_unchanged + st->rows_modified + st->rows_deleted != n_src) __builtin_trap();
    if (st->rows_moved > st->rows_unchanged + st->rows_modified) __builtin_trap();
}

static ibha_csvd_parser *parse_side(ibha_csvd_ctx *ctx, const uint8_t *body, size_t len,
                                    int keyed, const ibha_csvd_parser *expect) {
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    if (!keyed) {
        /* No header at all: every row is data and every column is part of the
         * key, which is the all-keys path of spec 6.4. */
        o.header.rows = 0;
        o.header.key_row = 0;
        o.header.required_row = 0;
        o.header.type_row = 0;
        o.header.name_row = 0;
    } else if (expect) {
        o.header.rows = IBHA_CSVD_HEADER_AUTO;
        o.expect_table = ibha_csvd_table_of(expect);
        o.expect_schema = ibha_csvd_schema_of(expect);
    }

    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
    if (!p) return NULL;
    if (keyed &&
        ibha_csvd_parse_chunk(p, k_keyed_header, sizeof(k_keyed_header) - 1) != IBHA_CSVD_OK) {
        return NULL;
    }
    if (len && ibha_csvd_parse_chunk(p, body, len) != IBHA_CSVD_OK) return NULL;
    if (ibha_csvd_parse_finish(p) != IBHA_CSVD_OK) return NULL;
    return p;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3) return 0;

    /* The first three bytes steer the harness rather than the parser: which
     * matching path to take, where to cut the input into two sides, and the diff
     * options to use. */
    int keyed = (data[0] & 1) != 0;
    size_t cut = 1 + ((size_t)data[1] * (size - 2)) / 256u;
    uint8_t knobs = data[2];
    data += 3;
    size -= 3;
    if (cut > size) cut = size;

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    if (!ctx) return 0;

    ibha_csvd_parser *sp = parse_side(ctx, data, cut, keyed, NULL);
    ibha_csvd_parser *tp = sp ? parse_side(ctx, data + cut, size - cut, keyed, keyed ? sp : NULL)
                              : NULL;

    if (sp && tp) {
        ibha_csvd_diff_opts o;
        ibha_csvd_diff_opts_init(&o);
        o.detect_moves = (knobs & 1) != 0;
        o.source_ordered = (knobs & 2) != 0;
        o.count_suppressed = (knobs & 4) != 0;
        o.deleted_placement =
            (knobs & 8) ? IBHA_CSVD_DELETED_END : IBHA_CSVD_DELETED_ANCHORED;
        o.similarity_k = (uint32_t)(knobs >> 4) + 1u;
        o.similarity_percent = (uint32_t)(knobs & 0x7Fu);

        ibha_csvd_diff *d =
            ibha_csvd_diff_run(ctx, ibha_csvd_table_of(sp), ibha_csvd_schema_of(sp),
                               ibha_csvd_table_of(tp), ibha_csvd_schema_of(tp), &o);
        if (d) {
            check_report(d, ibha_csvd_table_of(sp), ibha_csvd_schema_of(sp),
                         ibha_csvd_table_of(tp), ibha_csvd_schema_of(tp));
        }
    }

    ibha_csvd_ctx_free(ctx);
    return 0;
}
