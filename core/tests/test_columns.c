/*
 * test_columns.c - the column policy of spec 6.6 and 13.10.
 *
 * The decision this suite pins down: an added or removed column is an error by
 * default and a finding when the caller asks for that, per column kind, at the
 * call site.
 *
 * Three things the flags must never relax, and each has an assertion here because
 * each is the kind of leniency that would quietly produce a wrong report rather
 * than a failed one:
 *
 *   - **reordering stays a hard error.** Spec 13.10 has no flag for it, and a
 *     report built by comparing the wrong pairs of columns is worse than no
 *     report;
 *   - **a missing KEY column stays a hard error**, because the key is what row
 *     matching is built on;
 *   - **a file with no column name row cannot use the flags at all**, because
 *     without names there is nothing to tell an added column from a shifted one.
 *
 * And the property that makes the whole thing worth having: with a column
 * tolerated, the columns the two files *do* share must still diff exactly as they
 * would have if the extra column had never existed.
 */
#include <string.h>

#include "diffkit.h"
#include "suites.h"
#include "tap.h"

/* Four columns: id is KEY, then name, qty and note. */
#define H4 "KEY,,,\nREQUIRED,,,\nVARCHAR(10),VARCHAR(20),INTEGER,VARCHAR(30)\nid,name,qty,note\n"
/* The same file with a column appended by a spreadsheet user. */
#define H4_EXTRA                                                                 \
    "KEY,,,,\nREQUIRED,,,,\nVARCHAR(10),VARCHAR(20),INTEGER,VARCHAR(30),VARCHAR(9)\n" \
    "id,name,qty,note,extra\n"
/* The same file with the note column dropped. */
#define H3_FEWER "KEY,,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\n"

static int run_policy(kit_pair *p, const char *src, const char *tgt, int allow_added,
                      int allow_removed) {
    ibha_csvd_compare_opts cmp;
    ibha_csvd_compare_opts_init(&cmp);
    cmp.allow_added_columns = allow_added;
    cmp.allow_removed_columns = allow_removed;

    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.compare = cmp;
    return kit_parse(p, src, tgt, 4, &cmp) && kit_diff(p, &o);
}

/* ------------------------------------------------------- still an error -- */

static void test_default_is_an_error(void) {
    kit_pair p;

    if (!run_policy(&p, H4 "A,alice,1,n\n", H4_EXTRA "A,alice,1,n,x\n", 0, 0)) {
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_COLUMN_ORDER,
                   "an added column is an error by default");
    } else {
        TAP_OK(0, "an added column is an error by default");
    }
    kit_close(&p);

    if (!run_policy(&p, H4 "A,alice,1,n\n", H3_FEWER "A,alice,1\n", 0, 0)) {
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_COLUMN_ORDER,
                   "a removed column is an error by default");
    } else {
        TAP_OK(0, "a removed column is an error by default");
    }
    kit_close(&p);

    /* The two flags are separate because the two cases are not equally serious.
     * Allowing an appended column must not quietly allow a dropped one. */
    if (!run_policy(&p, H4 "A,alice,1,n\n", H3_FEWER "A,alice,1\n", 1, 0)) {
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_COLUMN_ORDER,
                   "allowing an added column does not allow a removed one");
    } else {
        TAP_OK(0, "allowing an added column does not allow a removed one");
    }
    kit_close(&p);

    if (!run_policy(&p, H4 "A,alice,1,n\n", H4_EXTRA "A,alice,1,n,x\n", 0, 1)) {
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_COLUMN_ORDER,
                   "and allowing a removed column does not allow an added one");
    } else {
        TAP_OK(0, "and allowing a removed column does not allow an added one");
    }
    kit_close(&p);
}

static void test_what_the_flags_never_relax(void) {
    kit_pair p;

    /* Spec 13.10 has no flag for reordering, and these do not add one. */
    static const char *const k_reordered =
        "KEY,,,\nREQUIRED,,,\nVARCHAR(20),VARCHAR(10),INTEGER,VARCHAR(30)\n"
        "name,id,qty,note\nalice,A,1,n\n";
    if (!run_policy(&p, H4 "A,alice,1,n\n", k_reordered, 1, 1)) {
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_COLUMN_ORDER,
                   "reordered columns stay an error with both flags on");
    } else {
        TAP_OK(0, "reordered columns stay an error with both flags on");
    }
    kit_close(&p);

    /*
     * A column swapped for a differently named one in the same position. By names
     * alone this is indistinguishable from a rename, so it needs *both* flags: it
     * is one removal and one addition, and the caller has to have accepted both
     * for it to go through.
     */
    static const char *const k_renamed =
        "KEY,,,\nREQUIRED,,,\nVARCHAR(10),VARCHAR(20),INTEGER,VARCHAR(30)\n"
        "id,customer,qty,note\nA,alice,1,n\n";
    if (!run_policy(&p, H4 "A,alice,1,n\n", k_renamed, 1, 0)) {
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_COLUMN_ORDER,
                   "a column swapped for a differently named one needs both flags");
    } else {
        TAP_OK(0, "a column swapped for a differently named one needs both flags");
    }
    kit_close(&p);

    if (run_policy(&p, H4 "A,alice,1,n\n", k_renamed, 1, 1)) {
        const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(p.diff);
        TAP_OK(s->columns_added == 1 && s->columns_removed == 1,
               "and with both, it is reported as one removed and one added, not as a rename");
    } else {
        TAP_OK(0, "and with both, it is reported as one removed and one added, not as a rename");
    }
    kit_close(&p);

    /* The key is what matching is built on, so losing it is fatal however lenient
     * the caller asked to be. */
    static const char *const k_no_key =
        "KEY,,\nREQUIRED,,\nVARCHAR(20),INTEGER,VARCHAR(30)\nname,qty,note\nalice,1,n\n";
    if (!run_policy(&p, H4 "A,alice,1,n\n", k_no_key, 1, 1)) {
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_MISSING_KEY_COLUMN,
                   "a missing KEY column is an error whatever the policy");
    } else {
        TAP_OK(0, "a missing KEY column is an error whatever the policy");
    }
    kit_close(&p);

    /*
     * Without column names there is no way to tell an added column from a shifted
     * one, so the flags cannot apply and the count mismatch stays an error.
     * Guessing here would silently compare the wrong pairs of cells.
     */
    ibha_csvd_compare_opts cmp;
    ibha_csvd_compare_opts_init(&cmp);
    cmp.allow_added_columns = 1;
    cmp.allow_removed_columns = 1;

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parse_opts po;
    kit_opts(&po, 0);
    po.compare = cmp;
    ibha_csvd_parser *sp = ibha_csvd_parse_begin(ctx, &po);
    static const char k_a[] = "A,alice,1\n";
    static const char k_b[] = "A,alice,1,extra\n";
    ibha_csvd_parse_borrow(sp, k_a, sizeof(k_a) - 1);
    ibha_csvd_parse_finish(sp);

    ibha_csvd_parse_opts po2;
    kit_opts(&po2, 0);
    po2.compare = cmp;
    ibha_csvd_parser *tp = ibha_csvd_parse_begin(ctx, &po2);
    ibha_csvd_parse_borrow(tp, k_b, sizeof(k_b) - 1);
    ibha_csvd_parse_finish(tp);

    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.compare = cmp;
    ibha_csvd_diff *d = ibha_csvd_diff_run(ctx, ibha_csvd_table_of(sp), ibha_csvd_schema_of(sp),
                                           ibha_csvd_table_of(tp), ibha_csvd_schema_of(tp), &o);
    TAP_OK(d == NULL && ibha_csvd_ctx_status(ctx) == IBHA_CSVD_ERR_COLUMN_ORDER,
           "a file with no column name row cannot use the flags at all");
    ibha_csvd_ctx_free(ctx);
}

/* ------------------------------------------------------- what it becomes -- */

static void test_added_column(void) {
    kit_pair p;

    if (run_policy(&p, H4 "A,alice,1,n\nB,bob,2,m\n", H4_EXTRA "A,alicia,1,n,x\nB,bob,2,m,y\n", 1,
                   0)) {
        const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(p.diff);
        TAP_EQ_U64(s->columns_added, 1, "an added column is tolerated and counted");
        TAP_EQ_U64(s->columns_removed, 0, "with nothing reported as removed");
        TAP_EQ_U64(s->n_columns_compared, 4, "and the four shared columns are what is compared");

        /*
         * The point of the whole feature: the shared columns diff exactly as they
         * would have if the extra column had never existed. One name changed, and
         * the extra column's differing values must not turn row B into a
         * modification.
         */
        TAP_EQ_U64(s->rows_modified, 1, "only the row whose shared columns changed is modified");
        TAP_EQ_U64(s->rows_unchanged, 1, "and the row that differs only in the added column is not");

        /* Cell counts are the cursor's, so they mean something only once it has
         * been drained. */
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        while (c && ibha_csvd_cursor_next(c) == 1) { /* drain */
        }
        TAP_EQ_U64(s->cells_changed, 1, "one cell changed, in a shared column");
    } else {
        for (int i = 0; i < 6; i++) TAP_OK(0, "an added column is tolerated and counted");
    }
    kit_close(&p);

    /* A column inserted in the middle is the case that would misalign a
     * positional comparison, so it gets its own fixture. */
    static const char *const k_middle =
        "KEY,,,,\nREQUIRED,,,,\nVARCHAR(10),VARCHAR(9),VARCHAR(20),INTEGER,VARCHAR(30)\n"
        "id,inserted,name,qty,note\nA,zzz,alice,1,n\n";
    if (run_policy(&p, H4 "A,alice,1,n\n", k_middle, 1, 0)) {
        const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(p.diff);
        TAP_EQ_U64(s->columns_added, 1, "a column inserted in the middle is tolerated");
        TAP_EQ_U64(s->rows_unchanged, 1,
                   "and the shared columns still line up, so the row is unchanged");
    } else {
        TAP_OK(0, "a column inserted in the middle is tolerated");
        TAP_OK(0, "and the shared columns still line up, so the row is unchanged");
    }
    kit_close(&p);
}

static void test_removed_column(void) {
    kit_pair p;

    if (run_policy(&p, H4 "A,alice,1,n\nB,bob,2,m\n", H3_FEWER "A,alicia,1\nB,bob,2\n", 0, 1)) {
        const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(p.diff);
        TAP_EQ_U64(s->columns_removed, 1, "a removed column is tolerated and counted");
        TAP_EQ_U64(s->columns_added, 0, "with nothing reported as added");
        TAP_EQ_U64(s->n_columns_compared, 3, "and the three surviving columns are compared");
        TAP_EQ_U64(s->rows_modified, 1, "the row whose shared columns changed is modified");
        TAP_EQ_U64(s->rows_unchanged, 1, "and the other is unchanged");
    } else {
        for (int i = 0; i < 5; i++) TAP_OK(0, "a removed column is tolerated and counted");
    }
    kit_close(&p);

    /* Both at once: the uploaded file dropped one column and appended another. */
    static const char *const k_both =
        "KEY,,,\nREQUIRED,,,\nVARCHAR(10),VARCHAR(20),INTEGER,VARCHAR(9)\n"
        "id,name,qty,extra\nA,alice,1,x\n";
    if (run_policy(&p, H4 "A,alice,1,n\n", k_both, 1, 1)) {
        const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(p.diff);
        TAP_EQ_U64(s->columns_added, 1, "one column added and one removed, both tolerated");
        TAP_EQ_U64(s->columns_removed, 1, "and both counted");
        TAP_EQ_U64(s->rows_unchanged, 1, "with the three shared columns comparing equal");
    } else {
        for (int i = 0; i < 3; i++) TAP_OK(0, "one column added and one removed, both tolerated");
    }
    kit_close(&p);
}

/* --------------------------------------------- what the report looks like -- */

static void test_the_report(void) {
    kit_pair p;
    if (!run_policy(&p, H4 "A,alice,1,n\n", H4_EXTRA "A,alicia,1,n,secret\n", 1, 0)) {
        for (int i = 0; i < 5; i++) TAP_OK(0, "the report fixture diffs");
        kit_close(&p);
        return;
    }

    /* Report rows carry the compared columns only, so a consumer never sees a
     * column the two files do not share. */
    ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
    uint32_t n_columns = 0;
    if (c && ibha_csvd_cursor_next(c) == 1) n_columns = ibha_csvd_cursor_row(c)->n_columns;
    TAP_EQ_U64(n_columns, 4, "a report row carries the compared columns and no others");

    static char out[16384];
    ibha_csvd_buffer_sink bs;
    ibha_csvd_sink sink;
    ibha_csvd_emit_opts eo;
    uint32_t rows = 0;

    ibha_csvd_emit_opts_init(&eo, IBHA_CSVD_EMIT_SUMMARY);
    ibha_csvd_buffer_sink_init(&bs, out, sizeof(out) - 1);
    sink.write = ibha_csvd_buffer_sink_write;
    sink.ctx = &bs;
    memset(out, 0, sizeof(out));
    ibha_csvd_emit(p.diff, &eo, &sink, &rows);

    TAP_OK(strstr(out, "\"kind\":\"columnAdded\"") != NULL,
           "the summary reports the added column as a finding");
    TAP_OK(strstr(out, "\"name\":\"extra\"") != NULL, "naming the column, which is the useful part");
    TAP_OK(strstr(out, "\"added\":1") != NULL, "and counting it");
    TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_OK, "and none of it is an error");

    /* The added column's values must not leak into the report: a consumer that
     * renders every cell of a report row would otherwise show a column the source
     * never declared. */
    ibha_csvd_emit_opts_init(&eo, IBHA_CSVD_EMIT_JSONL);
    ibha_csvd_buffer_sink_init(&bs, out, sizeof(out) - 1);
    memset(out, 0, sizeof(out));
    ibha_csvd_emit(p.diff, &eo, &sink, &rows);
    TAP_OK(strstr(out, "secret") == NULL,
           "and the added column's values do not appear in the report rows");
    kit_close(&p);
}

void ibha_test_columns(void) {
    test_default_is_an_error();
    test_what_the_flags_never_relax();
    test_added_column();
    test_removed_column();
    test_the_report();
}
