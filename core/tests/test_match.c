/*
 * test_match.c - row matching, duplicate keys and move detection.
 *
 * Sections 6.1 to 6.4 of the spec, one suite per algorithm. The assertions that
 * matter most are the ones about what must *not* happen:
 *
 *  - moving one row to the top must report one moved row, not all of them
 *  - a duplicate key must stop the diff and name the key and both rows, because
 *    "duplicate key" without a location is useless in a 90,000 row file
 *  - with no declared key, three identical rows on each side must match three to
 *    three rather than becoming three additions and three deletions
 */
#include <stdio.h>
#include <string.h>

#include "diffkit.h"
#include "suites.h"
#include "tap.h"

/* Three columns, the first marked KEY. */
#define H3 "KEY,,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\n"
/* The same shape with no key column at all, which is the all-keys case of
 * spec 6.4: every column is part of the key. */
#define H3_NOKEY ",,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\n"

static void check_counts(kit_pair *p, uint32_t unchanged, uint32_t modified, uint32_t added,
                         uint32_t deleted, const char *name) {
    const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(p->diff);
    if (!s) {
        TAP_OK(0, name);
        return;
    }
    int ok = s->rows_unchanged == unchanged && s->rows_modified == modified &&
             s->rows_added == added && s->rows_deleted == deleted;
    TAP_OK(ok, name);
    if (!ok) {
        printf("  # got %u/%u/%u/%u want %u/%u/%u/%u (U/M/A/D)\n", s->rows_unchanged,
               s->rows_modified, s->rows_added, s->rows_deleted, unchanged, modified, added,
               deleted);
    }
}

/* ------------------------------------------------- keyed matching, spec 6.1 -- */

static void test_keyed_basics(void) {
    kit_pair p;
    int ok = kit_run(&p, H3 "A,alice,1\nB,bob,2\nC,carol,3\n",
                     H3 "A,alice,1\nB,bobby,2\nD,dave,4\n", 4);
    TAP_OK(ok, "a keyed diff runs");
    if (ok) {
        check_counts(&p, 1, 1, 1, 1, "one unchanged, one modified, one added, one deleted");
        char rep[64];
        kit_report(&p, rep, sizeof(rep));
        /* The deleted row C sat after B in the source, so anchored placement puts
         * it after B in the report rather than at the bottom. Spec 6.5. */
        TAP_EQ_STR(rep, "UMDA", "the report follows target order with the deletion anchored");
    }
    kit_close(&p);
}

static void test_reorder_is_not_a_change(void) {
    /* The first sub question of spec 6.1: a row that moved is found by key
     * wherever it now sits, so reordering a keyed file changes no cell. */
    kit_pair p;
    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.detect_moves = 0;

    int ok = kit_parse(&p, H3 "A,a,1\nB,b,2\nC,c,3\n", H3 "C,c,3\nA,a,1\nB,b,2\n", 4, NULL) &&
             kit_diff(&p, &o);
    TAP_OK(ok, "a wholly reordered keyed file diffs");
    if (ok) check_counts(&p, 3, 0, 0, 0, "and reports every row as unchanged");
    kit_close(&p);
}

static void test_deleted_placement_edges(void) {
    kit_pair p;
    char rep[64];

    if (kit_run(&p, H3 "A,a,1\nB,b,2\nC,c,3\n", H3 "B,b,2\nC,c,3\n", 4)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "DUU", "a row deleted from the front is emitted before the first row");
    } else {
        TAP_OK(0, "a row deleted from the front is emitted before the first row");
    }
    kit_close(&p);

    if (kit_run(&p, H3 "A,a,1\nB,b,2\nC,c,3\n", H3 "A,a,1\nB,b,2\n", 4)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "UUD", "a row deleted from the end is emitted last");
    } else {
        TAP_OK(0, "a row deleted from the end is emitted last");
    }
    kit_close(&p);

    if (kit_run(&p, H3 "A,a,1\nB,b,2\nC,c,3\nD,d,4\n", H3 "A,a,1\nD,d,4\n", 4)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "UDDU", "consecutive deletions stay grouped in source order");
    } else {
        TAP_OK(0, "consecutive deletions stay grouped in source order");
    }
    kit_close(&p);

    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.deleted_placement = IBHA_CSVD_DELETED_END;
    if (kit_parse(&p, H3 "A,a,1\nB,b,2\nC,c,3\n", H3 "A,a,1\nC,c,3\n", 4, NULL) &&
        kit_diff(&p, &o)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "UUD", "'end' placement puts deletions in a block at the bottom");
    } else {
        TAP_OK(0, "'end' placement puts deletions in a block at the bottom");
    }
    kit_close(&p);
}

/* -------------------------------------------------- duplicate keys, spec 13.9 -- */

static void test_duplicate_keys(void) {
    kit_pair p;
    int parsed = kit_parse(&p, H3 "A,a,1\nB,b,2\nA,z,9\n", H3 "A,a,1\nB,b,2\n", 4, NULL);
    TAP_OK(parsed && !kit_diff(&p, NULL), "a duplicate key in the source stops the diff");
    TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_DUPLICATE_KEY, "with DUPLICATE_KEY");

    /* The message has to carry the key and both row numbers. Without them the
     * error is unactionable in a large file, which is why spec 13.9 spells the
     * text out. */
    const char *msg = ibha_csvd_ctx_error(p.ctx);
    TAP_OK(strstr(msg, "(A)") != NULL, "the message names the offending key");
    TAP_OK(strstr(msg, "5") != NULL && strstr(msg, "7") != NULL,
           "and both row numbers, one based over the whole file");
    kit_close(&p);

    parsed = kit_parse(&p, H3 "A,a,1\nB,b,2\n", H3 "A,a,1\nB,b,2\nB,z,9\n", 4, NULL);
    TAP_OK(parsed && !kit_diff(&p, NULL), "a duplicate key in the uploaded file stops it too");
    TAP_OK(strstr(ibha_csvd_ctx_error(p.ctx), "uploaded file") != NULL,
           "and the message says which side it was on");
    kit_close(&p);

    /* Two rows with the same key but different key *bytes* under normalization
     * are still duplicates, because the comparator is what defines the key. */
    parsed = kit_parse(&p, H3 "A,a,1\n A ,b,2\n", H3 "A,a,1\n", 4, NULL);
    TAP_OK(parsed && !kit_diff(&p, NULL),
           "keys that collide only after normalization are duplicates too");
    kit_close(&p);
}

static void test_missing_key_column(void) {
    kit_pair p;
    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.require_key = 1;

    int parsed = kit_parse(&p, H3_NOKEY "A,a,1\n", H3_NOKEY "A,a,1\n", 4, NULL);
    TAP_OK(parsed && !kit_diff(&p, &o), "require_key refuses a schema with no KEY column");
    TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_MISSING_KEY_COLUMN,
               "with MISSING_KEY_COLUMN");
    kit_close(&p);
}

/* ------------------------------------------------- move detection, spec 6.2 -- */

static void test_moves(void) {
    kit_pair p;
    char rep[64];

    /* The case the whole of spec 6.2 exists for: one row dragged to the top must
     * not report every row as moved. */
    if (kit_run(&p, H3 "A,a,1\nB,b,2\nC,c,3\nD,d,4\nE,e,5\n",
                H3 "E,e,5\nA,a,1\nB,b,2\nC,c,3\nD,d,4\n", 4)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "U>UUUU", "one row moved to the top reports exactly one moved row");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->rows_moved, 1, "and the count agrees");
    } else {
        TAP_OK(0, "one row moved to the top reports exactly one moved row");
        TAP_OK(0, "and the count agrees");
    }
    kit_close(&p);

    /* moveDistance is the shift in rank among matched rows, so the row that
     * travelled from position 4 to position 0 reports -4. */
    if (kit_run(&p, H3 "A,a,1\nB,b,2\nC,c,3\nD,d,4\nE,e,5\n",
                H3 "E,e,5\nA,a,1\nB,b,2\nC,c,3\nD,d,4\n", 4)) {
        ibha_csvd_cursor *c = ibha_csvd_cursor_open(p.diff);
        int got = 0;
        if (c && ibha_csvd_cursor_next(c) == 1) got = ibha_csvd_cursor_row(c)->move_distance;
        TAP_EQ_U64((uint64_t)(int64_t)got, (uint64_t)(int64_t)-4, "moveDistance reports the shift");
    } else {
        TAP_OK(0, "moveDistance reports the shift");
    }
    kit_close(&p);

    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.detect_moves = 0;
    if (kit_parse(&p, H3 "A,a,1\nB,b,2\nC,c,3\n", H3 "C,c,3\nA,a,1\nB,b,2\n", 4, NULL) &&
        kit_diff(&p, &o)) {
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "UUU", "with detect_moves off nothing is reported as moved");
    } else {
        TAP_OK(0, "with detect_moves off nothing is reported as moved");
    }
    kit_close(&p);

    /* Spec 6.7: an unordered source makes both move detection and anchored
     * deletion meaningless, so they degrade and the degradation is reported. */
    ibha_csvd_diff_opts u;
    ibha_csvd_diff_opts_init(&u);
    u.source_ordered = 0;
    if (kit_parse(&p, H3 "A,a,1\nB,b,2\nC,c,3\n", H3 "C,c,3\nA,a,1\n", 4, NULL) &&
        kit_diff(&p, &u)) {
        const ibha_csvd_diff_stats *s = ibha_csvd_diff_stats_of(p.diff);
        TAP_OK(s->moves_forced_off == 1 && s->rows_moved == 0,
               "an unordered source forces move detection off and says so");
        kit_report(&p, rep, sizeof(rep));
        TAP_EQ_STR(rep, "UUD", "and degrades deleted placement to 'end'");
    } else {
        TAP_OK(0, "an unordered source forces move detection off and says so");
        TAP_OK(0, "and degrades deleted placement to 'end'");
    }
    kit_close(&p);
}

/* ------------------------------------------------ the all keys case, spec 6.4 -- */

static void test_all_keys(void) {
    kit_pair p;

    /* Stage 1, exact multiset matching. Three identical rows on each side must
     * match three to three; treating them as a set would lose the multiplicity
     * and treating them as unmatchable would report six changes. */
    int ok = kit_run(&p, H3_NOKEY "X,alpha,1\nX,alpha,1\nX,alpha,1\n",
                     H3_NOKEY "X,alpha,1\nX,alpha,1\nX,alpha,1\n", 4);
    TAP_OK(ok, "the all-keys path runs when no column is marked KEY");
    if (ok) {
        TAP_OK(ibha_csvd_diff_stats_of(p.diff)->all_keys == 1, "and reports that it was taken");
        check_counts(&p, 3, 0, 0, 0, "three identical rows on each side match three to three");
    } else {
        TAP_OK(0, "and reports that it was taken");
        TAP_OK(0, "three identical rows on each side match three to three");
    }
    kit_close(&p);

    /* Stage 2, similarity pairing over the leftovers. Two of three columns match,
     * which is 66% and above the default 50% threshold, so the row is reported as
     * modified rather than as one addition plus one deletion. */
    ok = kit_run(&p, H3_NOKEY "X,alpha,1\nX,alpha,1\nX,alpha,1\n",
                 H3_NOKEY "X,alpha,1\nX,alpha,1\nX,alpha,2\n", 4);
    if (ok) {
        check_counts(&p, 2, 1, 0, 0, "a changed cell pairs by similarity instead of splitting");
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->paired_by_similarity, 1,
                   "and the pairing is counted");
    } else {
        TAP_OK(0, "a changed cell pairs by similarity instead of splitting");
        TAP_OK(0, "and the pairing is counted");
    }
    kit_close(&p);

    /* Below the threshold nothing is paired, which is the right answer: two rows
     * with nothing in common are an addition and a deletion, not an edit. */
    ok = kit_run(&p, H3_NOKEY "A,alpha,1\n", H3_NOKEY "Z,omega,9\n", 4);
    if (ok) {
        check_counts(&p, 0, 0, 1, 1, "rows with nothing in common stay added and deleted");
    } else {
        TAP_OK(0, "rows with nothing in common stay added and deleted");
    }
    kit_close(&p);

    /* Raising the threshold above what the pair scores must un-pair it, which is
     * what proves the threshold is actually consulted. */
    ibha_csvd_diff_opts o;
    ibha_csvd_diff_opts_init(&o);
    o.similarity_percent = 90;
    if (kit_parse(&p, H3_NOKEY "X,alpha,1\n", H3_NOKEY "X,alpha,2\n", 4, NULL) && kit_diff(&p, &o)) {
        check_counts(&p, 0, 0, 1, 1, "a similarity threshold above the score refuses the pair");
    } else {
        TAP_OK(0, "a similarity threshold above the score refuses the pair");
    }
    kit_close(&p);

    /* Duplicate rows are legitimate with no declared key, so the duplicate key
     * error of spec 13.9 must not fire on this path. */
    ok = kit_run(&p, H3_NOKEY "X,a,1\nX,a,1\n", H3_NOKEY "X,a,1\nX,a,1\n", 4);
    TAP_OK(ok && ibha_csvd_ctx_status(p.ctx) == IBHA_CSVD_OK,
           "identical rows are not a duplicate key error when no key is declared");
    kit_close(&p);
}

void ibha_test_match(void) {
    test_keyed_basics();
    test_reorder_is_not_a_change();
    test_deleted_placement_edges();
    test_duplicate_keys();
    test_missing_key_column();
    test_moves();
    test_all_keys();
}
