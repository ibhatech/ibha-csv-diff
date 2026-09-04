/*
 * test_parse.c - the RFC 4180 state machine and the columnar index.
 *
 * The cases here are the ones that decide whether a real file parses correctly:
 * the quoting forms, the three line endings and their mixtures, the boundary
 * conditions at end of file, and the structural errors. Resumability across
 * chunk boundaries is asserted both here, on the specific splits that are
 * interesting, and in test_property.c across every fixture at every stride.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/internal.h"
#include "suites.h"
#include "tap.h"

/* ---------------------------------------------------------------- helpers -- */

static void opts_names_only(ibha_csvd_parse_opts *o) {
    ibha_csvd_parse_opts_init(o);
    o->header.rows = 1;
    o->header.key_row = 0;
    o->header.required_row = 0;
    o->header.type_row = 0;
    o->header.name_row = 1;
}

/* Feeds the input in fixed size chunks, which is the whole point: every test
 * below could be run at stride 1 and must give the same answer. */
static ibha_csvd_parser *parse_chunked(ibha_csvd_ctx *ctx, const char *s, size_t len, size_t stride,
                                       const ibha_csvd_parse_opts *o) {
    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, o);
    if (!p) return NULL;
    for (size_t off = 0; off < len;) {
        size_t n = len - off < stride ? len - off : stride;
        if (ibha_csvd_parse_chunk(p, s + off, n) != IBHA_CSVD_OK) return p;
        off += n;
    }
    ibha_csvd_parse_finish(p);
    return p;
}

static ibha_csvd_parser *parse_str(ibha_csvd_ctx *ctx, const char *s, const ibha_csvd_parse_opts *o) {
    return parse_chunked(ctx, s, strlen(s), strlen(s) ? strlen(s) : 1, o);
}

static int cell_is(const ibha_csvd_table *t, uint32_t row, uint32_t col, const char *want) {
    uint32_t f = ibha_csvd_row_field(t, row, col);
    if (f == 0xFFFFFFFFu) return 0;
    return ibha_csvd_field_cmp_str(t, f, want) == 0;
}

static uint8_t cell_flags(const ibha_csvd_table *t, uint32_t row, uint32_t col) {
    uint32_t f = ibha_csvd_row_field(t, row, col);
    return f == 0xFFFFFFFFu ? 0 : t->field_flags[f];
}

/* ------------------------------------------------------------------ shape -- */

static void test_basic_shape(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx, "id,name,city\n1,Ann,Oslo\n2,Bo,Lima\n", &o);
    const ibha_csvd_table *t = ibha_csvd_table_of(p);

    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "a plain file parses");
    TAP_EQ_U64(t->n_rows, 3, "row count includes the header row");
    TAP_EQ_U64(t->n_columns, 3, "column count comes from the first row");
    TAP_EQ_U64(t->n_fields, 9, "field count is rows times columns");
    TAP_EQ_U64(t->row_first_field[t->n_rows], t->n_fields, "the row index ends with a sentinel");
    TAP_OK(cell_is(t, 1, 0, "1") && cell_is(t, 1, 1, "Ann") && cell_is(t, 2, 2, "Lima"),
           "cells land in the right places");
    TAP_EQ_U64(ibha_csvd_schema_of(p)->first_data_row, 1, "the data starts after the header row");

    ibha_csvd_ctx_free(ctx);
}

static void test_quoting_forms(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx,
                                    "a,b,c\n"
                                    "\"Smith, John\",\"O\"\"Brien\",\"line one\nline two\"\n"
                                    "plain,\"\",\"quoted plain\"\n",
                                    &o);
    const ibha_csvd_table *t = ibha_csvd_table_of(p);

    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "quoted forms parse");
    TAP_EQ_U64(t->n_rows, 3, "a quoted newline does not end the record");
    TAP_OK(cell_is(t, 1, 0, "Smith, John"), "a quoted delimiter is data");
    TAP_OK(cell_is(t, 1, 1, "O\"Brien"), "a \"\" pair is one quote in the logical value");
    TAP_OK(cell_is(t, 1, 2, "line one\nline two"), "a quoted newline is data");

    TAP_EQ_U64(cell_flags(t, 1, 1) & IBHA_CSVD_FIELD_HAS_ESCAPE, IBHA_CSVD_FIELD_HAS_ESCAPE,
               "HAS_ESCAPE is set where a \"\" was seen");
    TAP_EQ_U64(cell_flags(t, 1, 0) & IBHA_CSVD_FIELD_HAS_ESCAPE, 0,
               "HAS_ESCAPE is not set on a merely quoted field");
    TAP_EQ_U64(cell_flags(t, 1, 2) & IBHA_CSVD_FIELD_HAS_NEWLINE, IBHA_CSVD_FIELD_HAS_NEWLINE,
               "HAS_NEWLINE marks a cell the view must measure");
    TAP_EQ_U64(cell_flags(t, 2, 0) & IBHA_CSVD_FIELD_QUOTED, 0, "a bare field is not QUOTED");
    TAP_EQ_U64(cell_flags(t, 2, 2) & IBHA_CSVD_FIELD_QUOTED, IBHA_CSVD_FIELD_QUOTED,
               "a quoted field is QUOTED");
    TAP_EQ_U64(cell_flags(t, 2, 1) & IBHA_CSVD_FIELD_EMPTY, IBHA_CSVD_FIELD_EMPTY,
               "a quoted empty field is EMPTY");
    TAP_OK(cell_is(t, 2, 2, "quoted plain"),
           "quoting a field that did not need it changes nothing about its value");

    ibha_csvd_ctx_free(ctx);
}

static void test_line_endings(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    struct {
        const char *body;
        const char *label;
    } cases[] = {
        {"a,b\n1,2\n3,4\n", "LF"},
        {"a,b\r\n1,2\r\n3,4\r\n", "CRLF"},
        {"a,b\r1,2\r3,4\r", "bare CR"},
        {"a,b\r\n1,2\n3,4\r", "mixed endings in one file"},
        {"a,b\n1,2\n3,4", "no trailing newline"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        /* Stride 1 so every CRLF is split across a chunk boundary, which is the
         * case a non resumable parser gets wrong. */
        ibha_csvd_parser *p =
            parse_chunked(ctx, cases[i].body, strlen(cases[i].body), 1, &o);
        const ibha_csvd_table *t = ibha_csvd_table_of(p);

        int ok = ibha_csvd_ctx_status(ctx) == IBHA_CSVD_OK && t->n_rows == 3 &&
                 t->n_columns == 2 && cell_is(t, 2, 0, "3") && cell_is(t, 2, 1, "4");
        TAP_OK(ok, cases[i].label);
        ibha_csvd_ctx_free(ctx);
    }
}

static void test_edges_at_eof(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    struct {
        const char *body;
        uint32_t rows;
        uint32_t cols;
        const char *last;
        const char *label;
    } cases[] = {
        {"a,b,c\n1,2,\n", 2, 3, "", "a trailing empty field before a newline is a field"},
        {"a,b,c\n1,2,", 2, 3, "", "a trailing empty field at end of file is a field"},
        {"a,b,c\n1,2,\"\"", 2, 3, "", "a quoted empty field at end of file"},
        {"a,b,c\n1,2,\"x\"", 2, 3, "x", "a quoted field closed by end of file"},
        {"a,b,c\n1,2,\"x\" ", 2, 3, "x", "padding after a closing quote at end of file"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parser *p = parse_chunked(ctx, cases[i].body, strlen(cases[i].body), 1, &o);
        const ibha_csvd_table *t = ibha_csvd_table_of(p);

        int ok = ibha_csvd_ctx_status(ctx) == IBHA_CSVD_OK && t->n_rows == cases[i].rows &&
                 t->n_columns == cases[i].cols && cell_is(t, 1, 2, cases[i].last);
        TAP_OK(ok, cases[i].label);
        ibha_csvd_ctx_free(ctx);
    }
}

static void test_blank_lines(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx, "a,b\n1,2\n\n\r\n3,4\n\n", &o);
    const ibha_csvd_table *t = ibha_csvd_table_of(p);

    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "blank lines do not fail the parse");
    TAP_EQ_U64(t->n_rows, 3, "blank lines are not records");
    TAP_EQ_U64(ibha_csvd_parse_stats_of(p)->blank_lines, 3, "blank lines are counted");
    TAP_OK(cell_is(t, 2, 0, "3"), "the row after a blank line is intact");
    ibha_csvd_ctx_free(ctx);

    /* A single column file where an empty row is meant: writing "" says so, and
     * that is why skipping blank lines loses nothing. */
    ctx = ibha_csvd_ctx_new(NULL);
    p = parse_str(ctx, "a\nx\n\"\"\ny\n", &o);
    t = ibha_csvd_table_of(p);
    TAP_EQ_U64(t->n_rows, 4, "a quoted empty row is a record");
    TAP_OK(cell_is(t, 2, 0, ""), "and its value is empty");
    ibha_csvd_ctx_free(ctx);
}

static void test_bom(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);
    static const char body[] = "\xEF\xBB\xBFid,name\n1,Ann\n";

    /* Stride 1 splits the BOM itself, which is one of the boundaries spec 2.5
     * calls out explicitly. */
    for (size_t stride = 1; stride <= 4; stride++) {
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parser *p = parse_chunked(ctx, body, sizeof(body) - 1, stride, &o);
        const ibha_csvd_table *t = ibha_csvd_table_of(p);
        TAP_OK(ibha_csvd_ctx_status(ctx) == IBHA_CSVD_OK && t->n_rows == 2 && cell_is(t, 0, 0, "id"),
               "a UTF-8 BOM is stripped whatever the chunking");
        ibha_csvd_ctx_free(ctx);
    }

    /* A file that is only the first byte of a BOM is not a BOM. */
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx, "\xEF", &o);
    TAP_EQ_U64(ibha_csvd_table_of(p)->n_fields, 1, "a one byte file that looks like a BOM prefix is data");
    ibha_csvd_ctx_free(ctx);

    o.dialect.strip_bom = 0;
    ctx = ibha_csvd_ctx_new(NULL);
    p = parse_str(ctx, body, &o);
    TAP_OK(!cell_is(ibha_csvd_table_of(p), 0, 0, "id"), "strip_bom off keeps the BOM in the value");
    ibha_csvd_ctx_free(ctx);
}

static void test_non_utf8(void) {
    /* Spec 5.1: the engine is byte oriented. A diff tool that refuses to open a
     * Latin-1 file is useless. */
    ibha_csvd_parse_opts o;
    opts_names_only(&o);
    static const char body[] = "a,b\nCaf\xE9,\xFF\xFE\n";

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_chunked(ctx, body, sizeof(body) - 1, 1, &o);
    const ibha_csvd_table *t = ibha_csvd_table_of(p);

    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "invalid UTF-8 is not an error");
    TAP_EQ_U64(t->n_rows, 2, "invalid UTF-8 parses structurally");
    TAP_OK(cell_is(t, 1, 0, "Caf\xE9"), "the bytes are preserved exactly");
    ibha_csvd_ctx_free(ctx);
}

/* ----------------------------------------------------------------- errors -- */

static void test_structural_errors(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    parse_str(ctx, "a,b\n\"unterminated,2\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_UNTERMINATED_QUOTE,
               "a quote left open at end of file is an error");
    TAP_EQ_STR(ibha_csvd_ctx_error(ctx), "unterminated quoted field opened at row 2, field 1",
               "and the error names where the quote opened");
    ibha_csvd_ctx_free(ctx);

    ctx = ibha_csvd_ctx_new(NULL);
    parse_str(ctx, "a,b\n\"ab\"c,2\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_BAD_CONTENT,
               "content after a closing quote is an error rather than silently dropped");
    ibha_csvd_ctx_free(ctx);

    /* But padding is not, because `"ab" ,c` is a real thing writers emit. */
    ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx, "a,b\n\"ab\" \t,2\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK,
               "padding between a closing quote and the delimiter is accepted");
    TAP_OK(cell_is(ibha_csvd_table_of(p), 1, 0, "ab"), "and the padding is not part of the value");
    ibha_csvd_ctx_free(ctx);

    /* A quote inside a bare field is data, not a structural error. */
    ctx = ibha_csvd_ctx_new(NULL);
    p = parse_str(ctx, "a,b\nsay \"hi\",2\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "a quote inside a bare field is data");
    TAP_OK(cell_is(ibha_csvd_table_of(p), 1, 0, "say \"hi\""), "and is kept verbatim");
    ibha_csvd_ctx_free(ctx);
}

static void test_ragged(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    /* Spec 13.5, flagged assumption: a row with genuinely missing fields is an
     * error, because a diff of a broken file misleads. */
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    parse_str(ctx, "a,b,c\n1,2\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_RAGGED_ROW, "a short row is an error");
    TAP_EQ_STR(ibha_csvd_ctx_error(ctx), "row 2 has 2 fields, expected 3",
               "and the error names the row and both counts");
    ibha_csvd_ctx_free(ctx);

    ctx = ibha_csvd_ctx_new(NULL);
    parse_str(ctx, "a,b,c\n1,2,3,4\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_RAGGED_ROW,
               "an extra non empty field is an error");
    ibha_csvd_ctx_free(ctx);

    /* The exception: Excel routinely emits trailing empty columns. */
    ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx, "a,b,c\n1,2,3,,\n4,5,6\n", &o);
    const ibha_csvd_table *t = ibha_csvd_table_of(p);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK,
               "trailing empty fields are normalized rather than rejected");
    TAP_EQ_U64(t->n_fields, 9, "and the extra fields are dropped from the index");
    TAP_EQ_U64(ibha_csvd_parse_stats_of(p)->ragged_normalized, 1, "the normalization is counted");
    TAP_OK(cell_is(t, 2, 0, "4"), "the following row is unaffected");
    ibha_csvd_ctx_free(ctx);
}

static void test_limits(void) {
    ibha_csvd_limits lim;
    ibha_csvd_limits_init(&lim);
    lim.max_bytes = 16;

    ibha_csvd_parse_opts o;
    opts_names_only(&o);

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(&lim);
    parse_chunked(ctx, "a,b\n1,2\n3,4\n5,6\n7,8\n", 20, 4, &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_TOO_LARGE,
               "the byte limit trips during a streamed parse");
    ibha_csvd_ctx_free(ctx);

    ibha_csvd_limits_init(&lim);
    lim.max_rows = 2;
    ctx = ibha_csvd_ctx_new(&lim);
    parse_str(ctx, "a,b\n1,2\n3,4\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_INVALID_ARG, "the row limit is enforced");
    ibha_csvd_ctx_free(ctx);

    ibha_csvd_limits_init(&lim);
    lim.max_columns = 2;
    ctx = ibha_csvd_ctx_new(&lim);
    parse_str(ctx, "a,b,c\n1,2,3\n", &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_INVALID_ARG, "the column limit is enforced");
    ibha_csvd_ctx_free(ctx);
}

/* ------------------------------------------------------------- resumability -- */

static void test_borrow_matches_chunks(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);
    static const char body[] = "a,b\n\"x,1\",\"y\"\"z\"\n2,\"m\nn\"\n";

    ibha_csvd_ctx *c1 = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p1 = parse_chunked(c1, body, sizeof(body) - 1, 3, &o);

    ibha_csvd_ctx *c2 = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p2 = ibha_csvd_parse_begin(c2, &o);
    ibha_csvd_parse_borrow(p2, body, sizeof(body) - 1);
    ibha_csvd_parse_finish(p2);

    const ibha_csvd_table *a = ibha_csvd_table_of(p1);
    const ibha_csvd_table *b = ibha_csvd_table_of(p2);
    TAP_OK(ibha_tables_identical(a, b), "the zero copy path produces the same index as chunked input");
    TAP_OK(b->bytes == (const uint8_t *)body, "the zero copy path did not copy the bytes");

    ibha_csvd_ctx_free(c1);
    ibha_csvd_ctx_free(c2);
}

static void test_split_escape_pair(void) {
    /* The specific boundary spec 2.5 names: a chunk ending between the two
     * quotes of an escape pair. */
    static const char body[] = "a,b\n\"x\"\"y\",2\n";
    size_t len = sizeof(body) - 1;

    int all_ok = 1;
    for (size_t split = 1; split < len; split++) {
        ibha_csvd_parse_opts o;
        opts_names_only(&o);
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
        ibha_csvd_parse_chunk(p, body, split);
        ibha_csvd_parse_chunk(p, body + split, len - split);
        ibha_csvd_parse_finish(p);

        const ibha_csvd_table *t = ibha_csvd_table_of(p);
        if (ibha_csvd_ctx_status(ctx) != IBHA_CSVD_OK || t->n_rows != 2 ||
            !cell_is(t, 1, 0, "x\"y")) {
            all_ok = 0;
        }
        ibha_csvd_ctx_free(ctx);
    }
    TAP_OK(all_ok, "an escape pair parses identically however the chunk boundary falls in it");
}

static void test_hash_is_quoting_invariant(void) {
    /*
     * The property spec 5.2 exists for: the same data quoted three different
     * ways must produce the same row digests, or a spreadsheet round trip
     * reports the whole file as modified.
     */
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    o.header.rows = 2;
    o.header.key_row = 1;
    o.header.required_row = 0;
    o.header.type_row = 0;
    o.header.name_row = 2;

    const char *variants[] = {
        "KEY,,\nid,name,note\n1,O\"Brien,plain\n",
        "\"KEY\",\"\",\"\"\n\"id\",\"name\",\"note\"\n\"1\",\"O\"\"Brien\",\"plain\"\n",
        "KEY,,\nid,name,note\n\"1\",\"O\"\"Brien\",plain\n",
    };

    uint64_t key0 = 0, full0 = 0;
    int same = 1, keyed = 0;
    for (size_t i = 0; i < 3; i++) {
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parser *p = parse_str(ctx, variants[i], &o);
        const ibha_csvd_table *t = ibha_csvd_table_of(p);
        keyed = ibha_csvd_schema_of(p)->n_key_columns == 1;

        if (i == 0) {
            key0 = t->row_key_hash[2];
            full0 = t->row_full_hash[2];
        } else if (t->row_key_hash[2] != key0 || t->row_full_hash[2] != full0) {
            same = 0;
        }
        ibha_csvd_ctx_free(ctx);
    }
    TAP_OK(keyed, "the KEY marker row selects the key column");
    TAP_OK(same, "row digests are identical under three different quoting policies");
    TAP_OK(key0 != full0, "the key digest and the full row digest are distinct");
}

static void test_header_rows_zero(void) {
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    o.header.rows = 0;
    o.header.key_row = 0;
    o.header.required_row = 0;
    o.header.type_row = 0;
    o.header.name_row = 0;

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx, "1,2\n3,4\n", &o);
    const ibha_csvd_table *t = ibha_csvd_table_of(p);

    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "a headerless file parses");
    TAP_EQ_U64(ibha_csvd_schema_of(p)->first_data_row, 0, "every row is data");
    TAP_OK(t->row_full_hash[0] != 0 && t->row_full_hash[1] != 0, "and every row is digested");
    /* With no key columns declared the two digests coincide, which is what
     * spec 6.4's all-keys case means at the index level. */
    TAP_OK(t->row_key_hash[0] == t->row_full_hash[0],
           "with no key columns the key digest is the full row digest");
    ibha_csvd_ctx_free(ctx);
}

static void test_empty_input(void) {
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    parse_chunked(ctx, "", 0, 1, &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_NO_HEADER,
               "an empty file cannot supply the four header rows");
    TAP_EQ_STR(ibha_csvd_ctx_error(ctx), "expected 4 header rows, the file has 0 rows",
               "and says so plainly");
    ibha_csvd_ctx_free(ctx);

    ctx = ibha_csvd_ctx_new(NULL);
    o.header.rows = 0;
    o.header.key_row = o.header.required_row = o.header.type_row = o.header.name_row = 0;
    ibha_csvd_parser *p = parse_chunked(ctx, "", 0, 1, &o);
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "an empty file with no header model is fine");
    TAP_EQ_U64(ibha_csvd_table_of(p)->n_rows, 0, "and has no rows");
    ibha_csvd_ctx_free(ctx);
}

static void test_dialect(void) {
    ibha_csvd_parse_opts o;
    opts_names_only(&o);
    o.dialect.delimiter = '\t';

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_str(ctx, "a\tb\n1,5\t2\n", &o);
    const ibha_csvd_table *t = ibha_csvd_table_of(p);
    TAP_EQ_U64(t->n_columns, 2, "a tab delimited dialect splits on tabs");
    TAP_OK(cell_is(t, 1, 0, "1,5"), "and a comma is then ordinary data");
    ibha_csvd_ctx_free(ctx);

    ctx = ibha_csvd_ctx_new(NULL);
    o.dialect.quote = ',';
    o.dialect.delimiter = ',';
    TAP_OK(ibha_csvd_parse_begin(ctx, &o) == NULL,
           "a dialect whose delimiter equals its quote is rejected");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_INVALID_ARG, "with INVALID_ARG");
    ibha_csvd_ctx_free(ctx);
}

void ibha_test_parse(void) {
    test_basic_shape();
    test_quoting_forms();
    test_line_endings();
    test_edges_at_eof();
    test_blank_lines();
    test_bom();
    test_non_utf8();
    test_structural_errors();
    test_ragged();
    test_limits();
    test_borrow_matches_chunks();
    test_split_escape_pair();
    test_hash_is_quoting_invariant();
    test_header_rows_zero();
    test_empty_input();
    test_dialect();
}
