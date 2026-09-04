/*
 * test_validate.c - the validation findings of spec 13.5.
 *
 * The property these assertions exist to hold: a finding is *output*, not an
 * error. Nothing in here may abort a diff, every finding must survive to the
 * cursor, and a row that is unchanged must still report the findings its cells
 * carry, because "these two files agree and both are wrong" is exactly the answer
 * the comparison is being run for.
 */
#include <string.h>

#include "diffkit.h"
#include "suites.h"
#include "tap.h"

/* id is KEY, name is REQUIRED, amount is DECIMAL(5,2), qty is INTEGER. The
 * declared decimal carries a comma, so its cell is quoted like any other value
 * containing the delimiter: a type row is a CSV row too. */
#define HV                                                                          \
    "KEY,,,\nREQUIRED,REQUIRED,,\nVARCHAR(4),VARCHAR(5),\"DECIMAL(5,2)\",INTEGER\n" \
    "id,name,amount,qty\n"

/* The flags of the first report row, which every fixture here has exactly one
 * data row's worth of. */
static int first_row_flags(kit_pair *p, uint8_t *out, uint32_t n, uint32_t *n_findings) {
    ibha_csvd_cursor *c = ibha_csvd_cursor_open(p->diff);
    if (!c || ibha_csvd_cursor_next(c) != 1) return 0;
    const ibha_csvd_row *r = ibha_csvd_cursor_row(c);
    for (uint32_t i = 0; i < n && i < r->n_columns; i++) out[i] = r->cell_flags[i];
    if (n_findings) *n_findings = r->n_findings;
    return 1;
}

/* Runs one target row against a fixed clean source row and returns the flags. */
static int check_row(const char *target_row, uint8_t *flags, uint32_t *n_findings) {
    kit_pair p;
    int ok = 0;
    memset(flags, 0, 4);
    if (kit_run(&p, HV "A,alice,1.00,1\n", target_row, 4)) {
        ok = first_row_flags(&p, flags, 4, n_findings);
    }
    kit_close(&p);
    return ok;
}

static void test_findings(void) {
    uint8_t f[4];
    uint32_t n = 0;

    if (check_row(HV "A,,1.00,1\n", f, &n)) {
        TAP_OK((f[1] & IBHA_CSVD_CELL_REQUIRED_EMPTY) != 0,
               "an empty cell in a REQUIRED column is a finding");
        TAP_EQ_U64(n, 1, "and the row counts one cell with a finding");
    } else {
        TAP_OK(0, "an empty cell in a REQUIRED column is a finding");
        TAP_OK(0, "and the row counts one cell with a finding");
    }

    /* An empty cell in a column that is not REQUIRED is absent, not malformed.
     * Reporting it as an unparseable number would bury the findings that
     * matter. */
    if (check_row(HV "A,alice,,\n", f, &n)) {
        TAP_OK((f[2] & IBHA_CSVD_CELL_FINDING) == 0 && (f[3] & IBHA_CSVD_CELL_FINDING) == 0,
               "an empty optional cell is not reported as an unparseable number");
    } else {
        TAP_OK(0, "an empty optional cell is not reported as an unparseable number");
    }

    if (check_row(HV "A,abcdef,1.00,1\n", f, &n)) {
        TAP_OK((f[1] & IBHA_CSVD_CELL_TOO_LONG) != 0, "a value over its VARCHAR(5) is a finding");
    } else {
        TAP_OK(0, "a value over its VARCHAR(5) is a finding");
    }

    /* Characters, not bytes: a name with an accent in it is not suddenly too
     * long because UTF-8 spends two bytes on one letter. */
    if (check_row(HV "A,caf\xC3\xA9,1.00,1\n", f, &n)) {
        TAP_OK((f[1] & IBHA_CSVD_CELL_TOO_LONG) == 0,
               "a four character name in five bytes is not too long for VARCHAR(5)");
    } else {
        TAP_OK(0, "a four character name in five bytes is not too long for VARCHAR(5)");
    }

    if (check_row(HV "A,alice,abc,1\n", f, &n)) {
        TAP_OK((f[2] & IBHA_CSVD_CELL_NOT_NUMERIC) != 0,
               "a value that does not parse as its DECIMAL is a finding");
    } else {
        TAP_OK(0, "a value that does not parse as its DECIMAL is a finding");
    }

    if (check_row(HV "A,alice,1.234,1\n", f, &n)) {
        TAP_OK((f[2] & IBHA_CSVD_CELL_PRECISION) != 0,
               "three decimal places in a DECIMAL(5,2) is a finding");
    } else {
        TAP_OK(0, "three decimal places in a DECIMAL(5,2) is a finding");
    }

    if (check_row(HV "A,alice,12345.00,1\n", f, &n)) {
        TAP_OK((f[2] & IBHA_CSVD_CELL_PRECISION) != 0,
               "five integer digits in a DECIMAL(5,2) is a finding");
    } else {
        TAP_OK(0, "five integer digits in a DECIMAL(5,2) is a finding");
    }

    /* The declared scale takes no part in equality, so 1.50 and 1.5 are the same
     * value, and neither violates DECIMAL(5,2). */
    if (check_row(HV "A,alice,1.50,1\n", f, &n)) {
        TAP_OK((f[2] & IBHA_CSVD_CELL_FINDING) == 0,
               "a trailing zero inside the declared scale is not a finding");
    } else {
        TAP_OK(0, "a trailing zero inside the declared scale is not a finding");
    }

    if (check_row(HV "A,alice,1.00,1.5\n", f, &n)) {
        TAP_OK((f[3] & IBHA_CSVD_CELL_PRECISION) != 0,
               "a fractional value in an INTEGER column is a finding");
    } else {
        TAP_OK(0, "a fractional value in an INTEGER column is a finding");
    }

    /* Several findings on one row, on different columns, all surviving. */
    if (check_row(HV "A,abcdef,zz,1\n", f, &n)) {
        TAP_OK((f[1] & IBHA_CSVD_CELL_TOO_LONG) && (f[2] & IBHA_CSVD_CELL_NOT_NUMERIC),
               "findings on different columns of one row do not displace each other");
        TAP_EQ_U64(n, 2, "and both cells are counted");
    } else {
        TAP_OK(0, "findings on different columns of one row do not displace each other");
        TAP_OK(0, "and both cells are counted");
    }
}

/* --------------------------------------------------- where they show up -- */

static void test_reporting(void) {
    kit_pair p;

    /*
     * The one that matters most. An unchanged row is decided from its digest and
     * its cells are never compared, so a finding on one is the case a fast path
     * could silently swallow. Both files agree that the name is missing, and the
     * report has to say so.
     */
    if (kit_run(&p, HV "A,,1.00,1\n", HV "A,,1.00,1\n", 4)) {
        uint8_t f[4];
        uint32_t n = 0;
        int ok = first_row_flags(&p, f, 4, &n);
        TAP_OK(ok && (f[1] & IBHA_CSVD_CELL_REQUIRED_EMPTY) != 0,
               "an unchanged row still reports the findings its cells carry");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->rows_unchanged, 1,
                   "and the row is still unchanged");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->cells_required_empty, 1,
                   "and the finding is counted in the summary");
    } else {
        TAP_OK(0, "an unchanged row still reports the findings its cells carry");
        TAP_OK(0, "and the row is still unchanged");
        TAP_OK(0, "and the finding is counted in the summary");
    }
    kit_close(&p);

    /* A deleted row has no target values, so its findings come from the source
     * side, which is the only side it reports. */
    if (kit_run(&p, HV "A,alice,1.00,1\nB,,1.00,1\n", HV "A,alice,1.00,1\n", 4)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        int ok = 0;
        while (c && ibha_csvd_cursor_next(c) == 1) {
            const ibha_csvd_row *r = ibha_csvd_cursor_row(c);
            if (r->kind != IBHA_CSVD_ROW_DELETED) continue;
            ok = (r->cell_flags[1] & IBHA_CSVD_CELL_REQUIRED_EMPTY) != 0 && r->n_findings == 1;
        }
        TAP_OK(ok, "a deleted row is validated against the values it reports, the source's");
    } else {
        TAP_OK(0, "a deleted row is validated against the values it reports, the source's");
    }
    kit_close(&p);

    /* Findings are never errors. The diff completes and the context stays OK. */
    if (kit_run(&p, HV "A,alice,1.00,1\n", HV "A,,zzzz,9.9\n", 4)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        while (c && ibha_csvd_cursor_next(c) == 1) { /* drain */
        }
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_OK,
                   "three findings on one row leave the context unpoisoned");
        TAP_OK(ibha_csvd_diff_stats_of(p.diff)->rows_with_findings == 1,
               "and the row is counted once however many cells it has");
    } else {
        TAP_OK(0, "three findings on one row leave the context unpoisoned");
        TAP_OK(0, "and the row is counted once however many cells it has");
    }
    kit_close(&p);
}

/* --------------------------------------------------------- what it costs -- */

static void test_cost(void) {
    kit_pair p;
    ibha_csvd_diff_opts o;

    ibha_csvd_diff_opts_init(&o);
    o.validate = 0;
    if (kit_parse(&p, HV "A,alice,1.00,1\n", HV "A,,zz,1\n", 4, NULL) && kit_diff(&p, &o)) {
        uint8_t f[4];
        uint32_t n = 9;
        int ok = first_row_flags(&p, f, 4, &n);
        TAP_OK(ok && (f[1] & IBHA_CSVD_CELL_FINDING) == 0 &&
                   (f[2] & IBHA_CSVD_CELL_FINDING) == 0 && n == 0,
               "validate off produces no findings at all");
    } else {
        TAP_OK(0, "validate off produces no findings at all");
    }
    kit_close(&p);

    /*
     * A names-only file declares no REQUIRED column, no length and no type, so
     * there is nothing to check and the pre-filter must turn the whole pass off
     * rather than walking every cell to discover that.
     */
    if (kit_run(&p, "id,name\nA,alice\n", "id,name\nA,alice\n", 1)) {
        uint8_t f[2];
        uint32_t n = 9;
        int ok = first_row_flags(&p, f, 2, &n);
        TAP_OK(ok && n == 0, "a schema with nothing to validate produces no findings");
    } else {
        TAP_OK(0, "a schema with nothing to validate produces no findings");
    }
    kit_close(&p);

    /* Draining still allocates nothing: the findings ride on the cursor's own
     * row buffer, which was allocated when it opened. */
    if (kit_run(&p, HV "A,alice,1.00,1\nB,,x,2\n", HV "A,alice,1.00,1\nB,,x,2\n", 4)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        uint64_t before = ibha_csvd_ctx_bytes_reserved(p.ctx);
        while (c && ibha_csvd_cursor_next(c) == 1) { /* drain */
        }
        TAP_EQ_U64(ibha_csvd_ctx_bytes_reserved(p.ctx), before,
                   "validating as the cursor advances allocates nothing");
    } else {
        TAP_OK(0, "validating as the cursor advances allocates nothing");
    }
    kit_close(&p);
}

void ibha_test_validate(void) {
    test_findings();
    test_reporting();
    test_cost();
}
