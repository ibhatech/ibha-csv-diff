/*
 * fuzz_emit.c - fuzz target for the emitters.
 *
 * The parser is fuzzed because untrusted bytes drive its control flow. The
 * emitters are fuzzed for a different reason: **untrusted bytes come back out of
 * them**, into a browser, and the escaping that stands between the two is the
 * kind of code that is correct on the twenty inputs someone thought of and wrong
 * on the twenty-first.
 *
 * So this target does not merely run the emitters. It re-checks their output with
 * the independent checkers in emitkit.h, on every input:
 *
 *   - every '<' in the HTML opens one of the tags this emitter may write, and
 *     every '&' opens one of five entities: cell content never became markup
 *   - every JSONL line is one complete JSON value, in valid UTF-8: a cell value
 *     never broke out of its string and turned one row into two
 *   - the summary is one JSON object however malformed the inputs were
 *   - no emitter writes more rows than the report has
 *
 *   make fuzz
 *   build/fuzz_emit -max_total_time=60 corpus/
 */
#include <stddef.h>
#include <stdint.h>

#include "../src/internal.h"
#include "emitkit.h"

/* A keyed header, so the keyed path is reachable and the declared types are
 * present: the validation findings that ride on a report row only exist when a
 * column declares something to violate. */
static const char k_header[] =
    "KEY,,\nREQUIRED,,\nVARCHAR(4),VARCHAR(8),INTEGER\nid,name,qty\n";

/* The same columns with one appended, so the column projection of spec 6.6 is
 * reachable. Random bytes will not produce a plausible header of their own, and
 * an unreached code path is an unfuzzed one. */
static const char k_header_wide[] =
    "KEY,,,\nREQUIRED,,,\nVARCHAR(4),VARCHAR(8),INTEGER,VARCHAR(4)\nid,name,qty,extra\n";

/* Bounded on purpose: a fuzz input is a few kilobytes, so its report is far
 * smaller than this, and an overflow means the invariants are checked against a
 * truncated document, which would be checking nothing. */
#define FUZZ_OUT_CAP (1u << 20)
static char g_out[FUZZ_OUT_CAP];

static size_t emit_into(ibha_csvd_diff *d, const ibha_csvd_emit_opts *o, uint32_t *rows) {
    ibha_csvd_buffer_sink bs;
    ibha_csvd_sink sink;

    ibha_csvd_buffer_sink_init(&bs, g_out, FUZZ_OUT_CAP);
    sink.write = ibha_csvd_buffer_sink_write;
    sink.ctx = &bs;
    if (ibha_csvd_emit(d, o, &sink, rows) != IBHA_CSVD_OK) return 0;
    if (bs.overflow) return 0;
    return bs.len;
}

static void check_emitters(ibha_csvd_diff *d, uint8_t knobs) {
    const ibha_csvd_diff_stats *st = ibha_csvd_diff_stats_of(d);
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;
    size_t n;

    ibha_csvd_emit_opts_init(&o, IBHA_CSVD_EMIT_HTML);
    o.changes_only = (knobs & 1) != 0;
    o.include_values = (knobs & 2) != 0;
    o.cell_diff = (uint8_t)(knobs >> 6); /* 0..3, all four modes of spec 7 */
    o.max_cell_bytes = (knobs & 4) ? 8u : 0u;
    n = emit_into(d, &o, &rows);
    if (n && !ek_html_ok(g_out, n)) __builtin_trap();
    if (rows > st->report_rows) __builtin_trap();

    ibha_csvd_emit_opts_init(&o, IBHA_CSVD_EMIT_JSONL);
    o.changes_only = (knobs & 1) != 0;
    o.include_values = (knobs & 2) != 0;
    o.max_cell_bytes = (knobs & 4) ? 8u : 0u;
    n = emit_into(d, &o, &rows);
    if (n) {
        if (ek_jsonl_ok(g_out, n) != (int)rows) __builtin_trap();
        if (rows && !ek_contains(g_out, n, "\"schemaVersion\":1")) __builtin_trap();
    }
    if (rows > st->report_rows) __builtin_trap();

    /* The CSV report has no independent checker beyond "it terminated", because
     * CSV is byte transparent by design: the property that matters there is the
     * formula guard, which is asserted in the unit suite where the input is
     * known. */
    ibha_csvd_emit_opts_init(&o, IBHA_CSVD_EMIT_CSV);
    o.csv_formula_guard = (knobs & 8) != 0;
    o.csv_delimiter = (knobs & 16) ? (uint8_t)';' : (uint8_t)',';
    (void)emit_into(d, &o, &rows);
    if (rows > st->report_rows) __builtin_trap();

    ibha_csvd_emit_opts_init(&o, IBHA_CSVD_EMIT_SUMMARY);
    n = emit_into(d, &o, &rows);
    if (n && ek_jsonl_ok(g_out, n) != 1) __builtin_trap();
}

static ibha_csvd_parser *parse_side(ibha_csvd_ctx *ctx, const uint8_t *body, size_t len,
                                    const ibha_csvd_parser *expect,
                                    const ibha_csvd_compare_opts *cmp, int wide) {
    const char *header = wide ? k_header_wide : k_header;
    size_t header_len = wide ? sizeof(k_header_wide) - 1 : sizeof(k_header) - 1;
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    o.compare = *cmp;
    if (expect) {
        o.header.rows = IBHA_CSVD_HEADER_AUTO;
        o.expect_table = ibha_csvd_table_of(expect);
        o.expect_schema = ibha_csvd_schema_of(expect);
    }

    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
    if (!p) return NULL;
    if (ibha_csvd_parse_chunk(p, header, header_len) != IBHA_CSVD_OK) return NULL;
    if (len && ibha_csvd_parse_chunk(p, body, len) != IBHA_CSVD_OK) return NULL;
    if (ibha_csvd_parse_finish(p) != IBHA_CSVD_OK) return NULL;
    return p;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3) return 0;

    size_t cut = 1 + ((size_t)data[0] * (size - 2)) / 256u;
    uint8_t knobs = data[1];
    uint8_t emit_knobs = data[2];
    data += 3;
    size -= 3;
    if (cut > size) cut = size;

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    if (!ctx) return 0;

    ibha_csvd_compare_opts cmp;
    ibha_csvd_compare_opts_init(&cmp);
    cmp.allow_added_columns = (knobs & 8) != 0;
    cmp.allow_removed_columns = (knobs & 16) != 0;

    /* Which side is the wide one matters: an added column and a removed one take
     * different branches of the projection. */
    int src_wide = (knobs & 32) != 0;
    int tgt_wide = (knobs & 64) != 0;

    ibha_csvd_parser *sp = parse_side(ctx, data, cut, NULL, &cmp, src_wide);
    ibha_csvd_parser *tp = sp ? parse_side(ctx, data + cut, size - cut, sp, &cmp, tgt_wide) : NULL;

    if (sp && tp) {
        ibha_csvd_diff_opts o;
        ibha_csvd_diff_opts_init(&o);
        o.compare = cmp;
        o.detect_moves = (knobs & 1) != 0;
        o.count_suppressed = (knobs & 2) != 0;
        o.validate = (knobs & 4) != 0;

        ibha_csvd_diff *d =
            ibha_csvd_diff_run(ctx, ibha_csvd_table_of(sp), ibha_csvd_schema_of(sp),
                               ibha_csvd_table_of(tp), ibha_csvd_schema_of(tp), &o);
        if (d) check_emitters(d, emit_knobs);
    }

    ibha_csvd_ctx_free(ctx);
    return 0;
}
