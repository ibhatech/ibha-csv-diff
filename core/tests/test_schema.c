/*
 * test_schema.c - the four header row model and target header auto-detection.
 *
 * Spec 13.8. The source is authoritative for every piece of schema metadata, and
 * the target's header row count is detected by looking for the row that is the
 * source's column names. The cases below are the four outcomes that detection
 * can have, plus the two mistakes it has to tell apart: a reordered header and a
 * resized one, which spec 13.10 makes hard errors.
 */
#include <string.h>

#include "../src/internal.h"
#include "suites.h"
#include "tap.h"

/* Four header rows, three columns, one key column, one data row. */
static const char k_source[] =
    "KEY,,\n"
    "REQUIRED,REQUIRED,\n"
    "VARCHAR(10),VARCHAR(20),INTEGER\n"
    "id,name,qty\n"
    "ACC-1,Ann,5\n";

static ibha_csvd_parser *parse_source(ibha_csvd_ctx *ctx) {
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
    ibha_csvd_parse_chunk(p, k_source, sizeof(k_source) - 1);
    ibha_csvd_parse_finish(p);
    return p;
}

/* Parses a target against that source with detection on, one byte at a time so
 * detection is exercised on a genuinely streamed input. */
static ibha_csvd_parser *parse_target(ibha_csvd_ctx *ctx, const ibha_csvd_parser *src,
                                      const char *body) {
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    o.header.rows = IBHA_CSVD_HEADER_AUTO;
    o.expect_table = ibha_csvd_table_of(src);
    o.expect_schema = ibha_csvd_schema_of(src);

    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, &o);
    if (!p) return NULL;
    size_t len = strlen(body);
    for (size_t i = 0; i < len; i++) {
        if (ibha_csvd_parse_chunk(p, body + i, 1) != IBHA_CSVD_OK) return p;
    }
    ibha_csvd_parse_finish(p);
    return p;
}

static void test_source_schema(void) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *p = parse_source(ctx);
    const ibha_csvd_schema *s = ibha_csvd_schema_of(p);

    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "a four header row source parses");
    TAP_EQ_U64(s->n_columns, 3, "the schema has three columns");
    TAP_EQ_U64(s->key_row, 0, "the KEY row is row 1");
    TAP_EQ_U64(s->type_row, 2, "the declared type row is row 3");
    TAP_EQ_U64(s->name_row, 3, "the column name row is row 4");
    TAP_EQ_U64(s->first_data_row, 4, "the data starts at row 5");
    TAP_EQ_U64(s->n_key_columns, 1, "one column is marked KEY");
    TAP_EQ_U64(s->col_flags[0] & IBHA_CSVD_COL_KEY, IBHA_CSVD_COL_KEY, "column 1 is the key");
    TAP_EQ_U64(s->col_flags[1] & IBHA_CSVD_COL_KEY, 0, "column 2 is not");
    TAP_EQ_U64(s->col_flags[1] & IBHA_CSVD_COL_REQUIRED, IBHA_CSVD_COL_REQUIRED,
               "column 2 is REQUIRED");
    TAP_EQ_U64(s->col_flags[2] & IBHA_CSVD_COL_REQUIRED, 0, "column 3 is not REQUIRED");

    /* Types stay where they are. Reading one is a comparison against the bytes
     * in place, never a materialized string. */
    uint32_t f = ibha_csvd_row_field(ibha_csvd_table_of(p), s->type_row, 2);
    TAP_OK(ibha_csvd_field_cmp_str(ibha_csvd_table_of(p), f, "INTEGER") == 0,
           "the declared type is readable in place");

    /* Header rows are not data and carry no digest. */
    TAP_EQ_U64(ibha_csvd_table_of(p)->row_full_hash[0], 0, "header rows are not digested");
    TAP_OK(ibha_csvd_table_of(p)->row_full_hash[4] != 0, "data rows are");

    ibha_csvd_ctx_free(ctx);
}

static void test_detection_outcomes(void) {
    struct {
        const char *body;
        uint32_t name_row;
        int names_only;
        const char *label;
    } cases[] = {
        {"id,name,qty\nACC-1,Ann,5\n", 0, 1, "a names-only target is detected at row 1"},
        {"KEY,,\nREQUIRED,REQUIRED,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\nACC-1,Ann,5\n", 3,
         0, "a four header row target is detected at row 4"},
        {"exported 2026-08-01,,\nid,name,qty\nACC-1,Ann,5\n", 1, 0,
         "a target with one metadata row is detected at row 2"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parser *src = parse_source(ctx);
        ibha_csvd_parser *tgt = parse_target(ctx, src, cases[i].body);
        const ibha_csvd_schema *s = ibha_csvd_schema_of(tgt);

        int ok = ibha_csvd_ctx_status(ctx) == IBHA_CSVD_OK && s->name_row == cases[i].name_row &&
                 s->first_data_row == cases[i].name_row + 1 && s->names_only == cases[i].names_only;
        TAP_OK(ok, cases[i].label);
        ibha_csvd_ctx_free(ctx);
    }
}

static void test_target_inherits_schema(void) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *src = parse_source(ctx);

    /* The target's own metadata rows disagree with the source: it marks the
     * wrong column as the key. Spec 13.8 says the source wins and the target's
     * opinion is never acted on. */
    ibha_csvd_parser *tgt = parse_target(ctx, src,
                                         ",KEY,\n"
                                         "REQUIRED,,\n"
                                         "VARCHAR(99),VARCHAR(99),VARCHAR(99)\n"
                                         "id,name,qty\n"
                                         "ACC-1,Ann,5\n");
    const ibha_csvd_schema *s = ibha_csvd_schema_of(tgt);
    const ibha_csvd_schema *ss = ibha_csvd_schema_of(src);

    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK, "a target with contradictory metadata parses");
    TAP_EQ_U64(s->n_key_columns, ss->n_key_columns, "the key column count comes from the source");
    TAP_EQ_U64(s->col_flags[0] & IBHA_CSVD_COL_KEY, IBHA_CSVD_COL_KEY,
               "the source's key column is the key");
    TAP_EQ_U64(s->col_flags[1] & IBHA_CSVD_COL_KEY, 0,
               "the target's own KEY marker is not acted on");
    TAP_EQ_U64(s->key_row, IBHA_CSVD_NO_ROW, "the target is recorded as carrying no schema rows");

    /* The payoff: identical data rows digest identically on both sides even
     * though the two files have different header shapes. */
    const ibha_csvd_table *a = ibha_csvd_table_of(src);
    const ibha_csvd_table *b = ibha_csvd_table_of(tgt);
    TAP_EQ_U64(a->row_key_hash[ss->first_data_row], b->row_key_hash[s->first_data_row],
               "the same row keys identically on both sides");
    TAP_EQ_U64(a->row_full_hash[ss->first_data_row], b->row_full_hash[s->first_data_row],
               "and digests identically despite the different header shapes");

    ibha_csvd_ctx_free(ctx);
}

static void test_detection_failures(void) {
    struct {
        const char *body;
        ibha_csvd_status want;
        const char *msg;
        const char *label;
    } cases[] = {
        {"a,b,c\n1,2,3\n", IBHA_CSVD_ERR_NO_HEADER, NULL,
         "a target with no recognizable header row fails with NO_HEADER"},
        {"name,id,qty\nAnn,ACC-1,5\n", IBHA_CSVD_ERR_COLUMN_ORDER,
         "column 1 of the uploaded file is \"name\", expected \"id\"; the columns are the same but "
         "reordered, which is not allowed",
         "a reordered header is reported as a reorder, not as a missing header"},
        {"id,name,qty,extra\nACC-1,Ann,5,x\n", IBHA_CSVD_ERR_COLUMN_ORDER,
         "the uploaded file has 4 columns, the source has 3; adding or removing a column is not "
         "allowed",
         "an added column is reported with both counts"},
        {"id,name\nACC-1,Ann\n", IBHA_CSVD_ERR_COLUMN_ORDER, NULL,
         "a removed column is an error too"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
        ibha_csvd_parser *src = parse_source(ctx);
        parse_target(ctx, src, cases[i].body);

        TAP_EQ_U64(ibha_csvd_ctx_status(ctx), cases[i].want, cases[i].label);
        if (cases[i].msg) TAP_EQ_STR(ibha_csvd_ctx_error(ctx), cases[i].msg, "  and the message");
        ibha_csvd_ctx_free(ctx);
    }

    /* Detection gives up after eight rows rather than scanning a whole file. */
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *src = parse_source(ctx);
    parse_target(ctx, src,
                 "x,y,z\nx,y,z\nx,y,z\nx,y,z\nx,y,z\nx,y,z\nx,y,z\nx,y,z\nid,name,qty\n1,2,3\n");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_NO_HEADER,
               "a header row below the eighth is not found, by design");
    ibha_csvd_ctx_free(ctx);
}

static void test_names_are_trimmed(void) {
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parser *src = parse_source(ctx);

    /* Padding around a column name is formatting, not a schema change. */
    ibha_csvd_parser *tgt = parse_target(ctx, src, " id , name ,\"qty\"\nACC-1,Ann,5\n");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_OK,
               "padding and quoting around a column name do not break detection");
    TAP_EQ_U64(ibha_csvd_schema_of(tgt)->names_only, 1, "and it is still a names-only file");
    ibha_csvd_ctx_free(ctx);

    /* A case change is a schema change and is not silently accepted. */
    ctx = ibha_csvd_ctx_new(NULL);
    src = parse_source(ctx);
    parse_target(ctx, src, "ID,name,qty\nACC-1,Ann,5\n");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_COLUMN_ORDER,
               "a column name that differs only in case is a schema mismatch");
    ibha_csvd_ctx_free(ctx);
}

static void test_auto_requires_a_source(void) {
    ibha_csvd_parse_opts o;
    ibha_csvd_parse_opts_init(&o);
    o.header.rows = IBHA_CSVD_HEADER_AUTO;

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    TAP_OK(ibha_csvd_parse_begin(ctx, &o) == NULL,
           "auto-detection without a source schema is rejected up front");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_INVALID_ARG, "with INVALID_ARG");
    ibha_csvd_ctx_free(ctx);
}

void ibha_test_schema(void) {
    test_source_schema();
    test_detection_outcomes();
    test_target_inherits_schema();
    test_detection_failures();
    test_names_are_trimmed();
    test_auto_requires_a_source();
}
