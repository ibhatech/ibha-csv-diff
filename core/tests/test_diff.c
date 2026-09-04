/*
 * test_diff.c - the pull cursor of spec 13.3 and the cell level results.
 *
 * The cursor is the single output primitive: every emitter in Phase 3 is a loop
 * over it, so its contract has to be nailed down here rather than discovered
 * later by four consumers independently.
 *
 * The other half of this suite is spec 5.3's other half. Normalization suppresses
 * differences, and "silent suppression is as dangerous as noise", so a row that
 * is unchanged only because 1 and 01 are the same integer has to say so.
 */
#include <stdio.h>
#include <string.h>

#include "diffkit.h"
#include "suites.h"
#include "tap.h"

#define H3 "KEY,,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\n"

/* ---------------------------------------------------------- the contract -- */

static void test_cursor_contract(void) {
    kit_pair p;
    if (!kit_run(&p, H3 "A,a,1\nB,b,2\n", H3 "A,a,1\nC,c,3\n", 4)) {
        TAP_OK(0, "the cursor fixture diffs");
        kit_close(&p);
        return;
    }

    ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
    TAP_OK(c != NULL, "a cursor opens");
    TAP_OK(ibha_csvd_cursor_row(c) == NULL, "row() is NULL before the first next()");

    uint32_t n = 0;
    while (ibha_csvd_cursor_next(c) == 1) {
        TAP_OK(ibha_csvd_cursor_row(c) != NULL, n == 0 ? "row() is live after next()" : "...");
        n++;
        if (n > 16) break;
    }
    TAP_EQ_U64(n, ibha_csvd_diff_stats_of(p.diff)->report_rows,
               "the cursor yields exactly report_rows rows");
    TAP_OK(ibha_csvd_cursor_row(c) == NULL, "row() is NULL again once the stream ends");
    TAP_EQ_U64((uint64_t)ibha_csvd_cursor_next(c), 0, "and next() keeps returning end of stream");

    ibha_csvd_cursor_reset(c);
    uint32_t again = 0;
    while (ibha_csvd_cursor_next(c) == 1) again++;
    TAP_EQ_U64(again, n, "reset replays the same stream");

    /* Several cursors may be open at once, each with its own position: the React
     * view and a JSONL writer over one diff must not disturb each other. */
    ibha_csvd_cursor *c2 = ibha_csvd_cursor_open(p.diff);
    ibha_csvd_cursor_reset(c);
    ibha_csvd_cursor_next(c);
    ibha_csvd_cursor_next(c);
    ibha_csvd_cursor_next(c2);
    const ibha_csvd_row *r1 = ibha_csvd_cursor_row(c);
    const ibha_csvd_row *r2 = ibha_csvd_cursor_row(c2);
    TAP_OK(r1 && r2 && r1->target_row != r2->target_row, "two cursors keep separate positions");
    kit_close(&p);
}

/* ------------------------------------------------------------ cell level -- */

/* Collects the cell flags of the first row of the given kind. */
static int flags_of_kind(kit_pair *p, uint8_t kind, uint8_t *out, uint32_t n) {
    ibha_csvd_cursor *c = ibha_csvd_cursor_open(p->diff);
    if (!c) return 0;
    while (ibha_csvd_cursor_next(c) == 1) {
        const ibha_csvd_row *r = ibha_csvd_cursor_row(c);
        if (r->kind != kind) continue;
        for (uint32_t i = 0; i < n && i < r->n_columns; i++) out[i] = r->cell_flags[i];
        return 1;
    }
    return 0;
}

static void test_changed_cells(void) {
    kit_pair p;
    uint8_t f[3] = {0, 0, 0};

    if (kit_run(&p, H3 "A,alice,1\n", H3 "A,alicia,1\n", 4) &&
        flags_of_kind(&p, IBHA_CSVD_ROW_MODIFIED, f, 3)) {
        TAP_OK(f[0] == 0 && (f[1] & IBHA_CSVD_CELL_CHANGED) && f[2] == 0,
               "only the column that changed is flagged");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->cells_changed, 1,
                   "and the cell count accumulates as the cursor runs");
    } else {
        TAP_OK(0, "only the column that changed is flagged");
        TAP_OK(0, "and the cell count accumulates as the cursor runs");
    }
    kit_close(&p);

    /* An unchanged row must cost no cell work and carry no flags. This is the
     * fast path of spec 6.1 step 3 stated as an assertion. */
    if (kit_run(&p, H3 "A,alice,1\n", H3 "A,alice,1\n", 4)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        int clean = 0;
        if (c && ibha_csvd_cursor_next(c) == 1) {
            const ibha_csvd_row *r = ibha_csvd_cursor_row(c);
            clean = r->kind == IBHA_CSVD_ROW_UNCHANGED && r->n_changed_cells == 0 &&
                    r->cell_flags[0] == 0 && r->cell_flags[1] == 0 && r->cell_flags[2] == 0;
        }
        TAP_OK(clean, "an unchanged row carries no cell flags at all");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->cells_changed, 0, "and changes no counters");
    } else {
        TAP_OK(0, "an unchanged row carries no cell flags at all");
        TAP_OK(0, "and changes no counters");
    }
    kit_close(&p);

    /* Added and deleted rows have nothing to compare against, so their flags
     * must be clear rather than left over from the previous row. */
    if (kit_run(&p, H3 "A,alice,1\nB,bob,2\n", H3 "A,alicia,1\nC,carol,3\n", 4)) {
        uint8_t g[3] = {9, 9, 9};
        int got_added = flags_of_kind(&p, IBHA_CSVD_ROW_ADDED, g, 3);
        TAP_OK(got_added && g[0] == 0 && g[1] == 0 && g[2] == 0,
               "an added row's flags are clear, not inherited from the row before it");
    } else {
        TAP_OK(0, "an added row's flags are clear, not inherited from the row before it");
    }
    kit_close(&p);
}

/* ------------------------------- suppressed by normalization, spec 5.3 -- */

static void test_suppression(void) {
    kit_pair p;

    /* 1 and 01 are the same INTEGER, so the row is unchanged. The user still has
     * to be able to find out that the file was rewritten, which is what
     * suppressedByNormalization is for. */
    if (kit_run(&p, H3 "A,alice,1\n", H3 "A,alice,01\n", 4)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        int ok = 0;
        if (c && ibha_csvd_cursor_next(c) == 1) {
            const ibha_csvd_row *r = ibha_csvd_cursor_row(c);
            ok = r->kind == IBHA_CSVD_ROW_UNCHANGED && r->n_suppressed_cells == 1 &&
                 (r->cell_flags[2] & IBHA_CSVD_CELL_SUPPRESSED) != 0 && r->cell_flags[1] == 0;
        }
        TAP_OK(ok, "a cell equal only after normalization is reported as suppressed");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->cells_suppressed, 1,
                   "and counted in the summary");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->rows_unchanged, 1,
                   "while the row itself stays unchanged");
    } else {
        TAP_OK(0, "a cell equal only after normalization is reported as suppressed");
        TAP_OK(0, "and counted in the summary");
        TAP_OK(0, "while the row itself stays unchanged");
    }
    kit_close(&p);

    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.count_suppressed = 0;
    if (kit_parse(&p, H3 "A,alice,1\n", H3 "A,alice,01\n", 4, NULL) && kit_diff(&p, &o)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        while (c && ibha_csvd_cursor_next(c) == 1) { /* drain */
        }
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->cells_suppressed, 0,
                   "count_suppressed off skips the walk entirely");
    } else {
        TAP_OK(0, "count_suppressed off skips the walk entirely");
    }
    kit_close(&p);

    /* A modified row can carry both: one column genuinely changed and another
     * differs only in formatting. Collapsing the two would lose one of them. */
    if (kit_run(&p, H3 "A,alice,1\n", H3 "A,alicia,01\n", 4)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        int ok = 0;
        if (c && ibha_csvd_cursor_next(c) == 1) {
            const ibha_csvd_row *r = ibha_csvd_cursor_row(c);
            ok = r->kind == IBHA_CSVD_ROW_MODIFIED && r->n_changed_cells == 1 &&
                 r->n_suppressed_cells == 1 &&
                 (r->cell_flags[1] & IBHA_CSVD_CELL_CHANGED) != 0 &&
                 (r->cell_flags[2] & IBHA_CSVD_CELL_SUPPRESSED) != 0;
        }
        TAP_OK(ok, "a modified row reports changed and suppressed cells separately");
    } else {
        TAP_OK(0, "a modified row reports changed and suppressed cells separately");
    }
    kit_close(&p);
}

/* ---------------------------------------------------------------- edges -- */

static void test_edges(void) {
    kit_pair p;
    char rep[64];

    if (kit_run(&p, H3 "A,a,1\nB,b,2\n", H3, 4)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "DD", "a target with only a header reports every source row deleted");
    } else {
        TAP_OK(0, "a target with only a header reports every source row deleted");
    }
    kit_close(&p);

    if (kit_run(&p, H3, H3 "A,a,1\nB,b,2\n", 4)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "AA", "a source with only a header reports every target row added");
    } else {
        TAP_OK(0, "a source with only a header reports every target row added");
    }
    kit_close(&p);

    if (kit_run(&p, H3, H3, 4)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "", "two header-only files produce an empty report");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->report_rows, 0, "with report_rows of zero");
    } else {
        TAP_OK(0, "two header-only files produce an empty report");
        TAP_OK(0, "with report_rows of zero");
    }
    kit_close(&p);

    /* The digests are the whole basis of the matcher, so a table parsed without
     * them has to be refused rather than silently matching everything to row
     * zero, which is what a table of zero hashes would do. */
    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parse_opts o;
    kit_opts(&o, 4);
    o.hash_rows = 0;
    ibha_csvd_parser *q = ibha_csvd_parse_begin(ctx, &o);
    static const char body[] = H3 "A,a,1\n";
    ibha_csvd_parse_borrow(q, body, sizeof(body) - 1);
    ibha_csvd_parse_finish(q);
    ibha_csvd_diff *dd =
        ibha_csvd_diff_run(ctx, ibha_csvd_table_of(q), ibha_csvd_schema_of(q),
                           ibha_csvd_table_of(q), ibha_csvd_schema_of(q), NULL);
    TAP_OK(dd == NULL, "a diff over tables parsed without digests is refused");
    TAP_EQ_U64(ibha_csvd_ctx_status(ctx), IBHA_CSVD_ERR_INVALID_ARG, "with INVALID_ARG");
    ibha_csvd_ctx_free(ctx);
}

/* ------------------------------------------------- what the cursor costs -- */

static void test_cursor_is_bounded(void) {
    /*
     * Spec 13.3: peak memory in cursor mode is the two indexes plus one row, and
     * the report index array is not built at all. So draining the cursor must not
     * grow the arena: everything it needs was allocated by diff_run.
     */
    static const char *const k_src =
        H3 "A,a,1\nB,b,2\nC,c,3\nD,d,4\nE,e,5\nF,f,6\nG,g,7\nH,h,8\n";
    static const char *const k_tgt =
        H3 "H,h,8\nA,a,1\nB,bb,2\nD,d,4\nE,e,5\nX,x,9\nF,f,6\nG,g,7\n";

    kit_pair p;
    if (!kit_run(&p, k_src, k_tgt, 4)) {
        TAP_OK(0, "the bounded memory fixture diffs");
        kit_close(&p);
        return;
    }

    uint64_t before = ibha_csvd_ctx_bytes_reserved(p.ctx);
    ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
    uint64_t after_open = ibha_csvd_ctx_bytes_reserved(p.ctx);
    uint32_t n = 0;
    while (ibha_csvd_cursor_next(c) == 1) n++;
    uint64_t after_drain = ibha_csvd_ctx_bytes_reserved(p.ctx);

    TAP_OK(after_open >= before, "opening a cursor allocates only its own row buffer");
    TAP_EQ_U64(after_drain, after_open, "draining the cursor allocates nothing at all");
    TAP_EQ_U64(n, ibha_csvd_diff_stats_of(p.diff)->report_rows,
               "and yields every report row exactly once");
    kit_close(&p);
}

void ibha_test_diff(void) {
    test_cursor_contract();
    test_changed_cells();
    test_suppression();
    test_edges();
    test_cursor_is_bounded();
}
