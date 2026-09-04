/*
 * test_normalize.c - the declared type comparators of spec 5.3.
 *
 * The assertions come in two halves and the second half is the one that matters.
 *
 * The first half checks each comparator: that DECIMAL makes 1.50 equal 1.5, that
 * INTEGER makes 007 equal 7, that BOOLEAN is case insensitive against the truth
 * set, that DATE is exact by default, and that anything the canonical forms
 * decline falls back to trimmed byte equality rather than to nonsense.
 *
 * The second half checks the coupling: **whenever the comparator says two cells
 * are equal, their row digests must agree**. That is the invariant the unchanged
 * row fast path of spec 6.1 step 3 rests on. If it ever fails, the engine reports
 * a row as modified and then finds every one of its cells identical.
 */
#include <stdio.h>
#include <string.h>

#include "diffkit.h"
#include "suites.h"
#include "tap.h"

/*
 * One data row under a two column schema: a keyed id column so the two sides
 * match as one row rather than as an addition and a deletion, and one value
 * column carrying the declared type under test.
 */
static void one_col(char *dst, size_t cap, const char *type, const char *value) {
    snprintf(dst, cap, "KEY,\n,REQUIRED\nVARCHAR(10),\"%s\"\nid,v\n1,%s\n", type, value);
}

/* Parses two such files and compares the value cell under its declared type.
 * Returns 1 when the comparator calls them equal. */
static int cmp_typed(const char *type, const char *a, const char *b, int *digests_agree) {
    char sa[512], sb[512];
    one_col(sa, sizeof(sa), type, a);
    one_col(sb, sizeof(sb), type, b);

    kit_pair p;
    if (!kit_parse(&p, sa, sb, 4, NULL)) {
        printf("  # parse failed for type %s: %s\n", type, ibha_csvd_ctx_error(p.ctx));
        kit_close(&p);
        *digests_agree = 0;
        return -1;
    }

    const ibha_csvd_table *ts = ibha_csvd_table_of(p.src);
    const ibha_csvd_table *tt = ibha_csvd_table_of(p.tgt);
    const ibha_csvd_schema *ss = ibha_csvd_schema_of(p.src);
    uint32_t row = ss->first_data_row;

    int eq = ibha_csvd_field_cmp_typed(ts, ts->row_first_field[row] + 1, tt,
                                       tt->row_first_field[row] + 1,
                                       (ibha_csvd_type)ss->col_type[1], NULL) == 0;
    /* The id column is identical on both sides, so the full row digests agree
     * exactly when the value cells do. */
    *digests_agree = ts->row_full_hash[row] == tt->row_full_hash[row];
    kit_close(&p);
    return eq;
}

/* The two facts are asserted together on purpose: a comparator that is right
 * while the digest disagrees is not a passing case, it is the exact bug the
 * digests were rewritten to prevent. */
static void expect(const char *type, const char *a, const char *b, int want_equal,
                   const char *name) {
    int digests = 0;
    int eq = cmp_typed(type, a, b, &digests);
    if (eq != want_equal) {
        TAP_OK(0, name);
        printf("  # %s: \"%s\" vs \"%s\" compared %s\n", type, a, b, eq ? "equal" : "unequal");
        return;
    }
    TAP_OK(eq == want_equal && digests == want_equal, name);
    if (digests != want_equal) {
        printf("  # %s: the comparator and the row digest disagree on \"%s\" vs \"%s\"\n", type, a,
               b);
    }
}

/* -------------------------------------------------------- declared types -- */

static void test_type_parsing(void) {
    static const char *const k_src =
        "KEY,,,,,,\n"
        ",,,,,,\n"
        "VARCHAR(20),\"DECIMAL(12,2)\",INTEGER,BOOLEAN,DATE, char(3) ,WIDGET\n"
        "a,b,c,d,e,f,g\n"
        "1,2,3,TRUE,2026-01-01,xy,z\n";

    kit_pair p;
    if (!kit_parse(&p, k_src, k_src, 4, NULL)) {
        TAP_OK(0, "the declared type row parses");
        kit_close(&p);
        return;
    }
    const ibha_csvd_schema *s = ibha_csvd_schema_of(p.src);

    TAP_EQ_U64(s->col_type[0], IBHA_CSVD_TYPE_VARCHAR, "VARCHAR(20) is recognized");
    TAP_EQ_U64(s->col_size[0], 20, "and carries its declared length");
    TAP_EQ_U64(s->col_type[1], IBHA_CSVD_TYPE_DECIMAL, "DECIMAL(12,2) is recognized");
    TAP_EQ_U64(s->col_size[1], 12, "with its precision");
    TAP_EQ_U64(s->col_scale[1], 2, "and its scale");
    TAP_EQ_U64(s->col_type[2], IBHA_CSVD_TYPE_INTEGER, "INTEGER is recognized");
    TAP_EQ_U64(s->col_scale[2], -1, "a type with no suffix reports no scale");
    TAP_EQ_U64(s->col_type[3], IBHA_CSVD_TYPE_BOOLEAN, "BOOLEAN is recognized");
    TAP_EQ_U64(s->col_type[4], IBHA_CSVD_TYPE_DATE, "DATE is recognized");
    /* DATE is a prefix of DATETIME, so matching on the whole identifier rather
     * than on a prefix is what stops a whole column getting the wrong
     * comparator. */
    TAP_EQ_U64(s->col_type[5], IBHA_CSVD_TYPE_CHAR, "lower case and padded type text still matches");
    TAP_EQ_U64(s->col_size[5], 3, "with its declared width");
    TAP_EQ_U64(s->col_type[6], IBHA_CSVD_TYPE_UNKNOWN, "an unrecognized type is UNKNOWN");
    kit_close(&p);

    static const char *const k_dt =
        "KEY,\n,\nDATETIME,TIMESTAMP\na,b\n2026-01-01,2026-01-01\n";
    kit_pair q;
    if (!kit_parse(&q, k_dt, k_dt, 4, NULL)) {
        TAP_OK(0, "DATETIME parses");
        kit_close(&q);
        return;
    }
    const ibha_csvd_schema *s2 = ibha_csvd_schema_of(q.src);
    TAP_EQ_U64(s2->col_type[0], IBHA_CSVD_TYPE_TIMESTAMP, "DATETIME is not read as DATE");
    TAP_EQ_U64(s2->col_type[1], IBHA_CSVD_TYPE_TIMESTAMP, "TIMESTAMP is recognized");
    kit_close(&q);
}

/* ------------------------------------------------------------ comparators -- */

static void test_decimal(void) {
    /* The Excel round trip, which is the case spec 5.3 exists for. */
    expect("DECIMAL(12,2)", "1.50", "1.5", 1, "DECIMAL: 1.50 equals 1.5");
    expect("DECIMAL(12,2)", "1.500", "1.5", 1, "DECIMAL: 1.500 equals 1.5");
    expect("DECIMAL(12,2)", "+1.5", "1.5", 1, "DECIMAL: a leading plus is not a difference");
    expect("DECIMAL(12,2)", "0001.5", "1.5", 1, "DECIMAL: leading zeros are not a difference");
    expect("DECIMAL(12,2)", "-0.00", "0", 1, "DECIMAL: negative zero is zero");
    expect("DECIMAL(12,2)", ".5", "0.5", 1, "DECIMAL: a bare leading point is the same value");
    expect("DECIMAL(12,2)", "5.", "5", 1, "DECIMAL: a bare trailing point is the same value");
    expect("DECIMAL(20,0)", "1.23457E+14", "123457000000000", 1,
           "DECIMAL: Excel's exponent form equals the plain form");

    /* And the differences that must survive. Truncating to the declared scale,
     * which is one reading of spec 5.3, would have merged these two. */
    expect("DECIMAL(12,2)", "1.555", "1.554", 0, "DECIMAL: extra scale is not rounded away");
    expect("DECIMAL(12,2)", "1.5", "-1.5", 0, "DECIMAL: sign is a difference");
    expect("DECIMAL(12,2)", "", "0", 0, "DECIMAL: empty is not zero");
    expect("DECIMAL(12,2)", "1.5", "1.5x", 0, "DECIMAL: trailing junk is not a number");
    /* Not a number on either side, so both fall back to trimmed bytes, which is
     * spec 5.3's stated fallback and still gives a usable answer. */
    expect("DECIMAL(12,2)", "n/a", "n/a", 1, "DECIMAL: a non numeric value still compares by bytes");
    expect("DECIMAL(12,2)", "n/a", "N/A", 0, "DECIMAL: and that fallback stays case sensitive");
}

static void test_integer(void) {
    expect("INTEGER", "007", "7", 1, "INTEGER: 007 equals 7");
    expect("BIGINT", "0", "-0", 1, "BIGINT: zero has no sign");
    expect("INTEGER", "7.0", "7", 1, "INTEGER: a spreadsheet's 7.0 equals 7");
    expect("INTEGER", "7", "8", 0, "INTEGER: different values still differ");
}

static void test_boolean(void) {
    expect("BOOLEAN", "TRUE", "True", 1, "BOOLEAN: case is not a difference");
    expect("BOOLEAN", "TRUE", "yes", 1, "BOOLEAN: the truth set has more than one spelling");
    expect("BOOLEAN", "1", "T", 1, "BOOLEAN: 1 is true");
    expect("BOOLEAN", "FALSE", "0", 1, "BOOLEAN: 0 is false");
    expect("BOOLEAN", "TRUE", "FALSE", 0, "BOOLEAN: true is not false");
    /* Neither is in a truth set, so nothing is coerced and they compare as text.
     * Silently mapping an unrecognized value to false would be a data error. */
    expect("BOOLEAN", "maybe", "MAYBE", 0, "BOOLEAN: an unrecognized value is not coerced");
}

static void test_text_and_dates(void) {
    expect("VARCHAR(20)", " abc ", "abc", 1, "VARCHAR: trimming is on by default");
    expect("VARCHAR(20)", "abc", "ABC", 0, "VARCHAR: case is a difference");
    expect("VARCHAR(20)", "a b", "a  b", 0, "VARCHAR: interior whitespace is data");
    expect("CHAR(5)", "ab   ", "ab", 1, "CHAR: trailing pad is not a difference");
    expect("DATE", "2026-01-05", "2026-01-05", 1, "DATE: exact comparison matches identical text");
    /* Spec 5.3 is explicit that guessing between 1/5/2024 and 5/1/2024 is not
     * something a library should do silently, so these must differ. */
    expect("DATE", "2026-01-05", "1/5/2026", 0, "DATE: exact is the default and does not guess");
    expect("WIDGET", " x ", "x", 1, "an unknown type compares as trimmed bytes");
}

/*
 * Group B item 2 of specs/03-remaining-tasks.md, confirmed by Manas 2026-09-04:
 * an absent fraction equals .000, and trailing zeros in a fraction are not
 * significant. The rule is ibha_canonical_decimal's, applied to the seconds.
 */
static void test_timestamp_fraction(void) {
    expect("TIMESTAMP", "2026-01-31T14:22:05", "2026-01-31T14:22:05.000", 1,
           "TIMESTAMP: no fraction equals an all zero fraction");
    expect("TIMESTAMP", "2026-01-31T14:22:05.000", "2026-01-31T14:22:05.0", 1,
           "TIMESTAMP: .000 equals .0");
    expect("TIMESTAMP", "2026-01-31T14:22:05.100", "2026-01-31T14:22:05.1", 1,
           "TIMESTAMP: trailing zeros in a fraction are not significant");
    expect("TIMESTAMP", "2026-01-31T14:22:05.123456000", "2026-01-31T14:22:05.123456", 1,
           "TIMESTAMP: and at any width");
    expect("TIMESTAMP", "2026-01-31 14:22:05.000", "2026-01-31 14:22:05", 1,
           "TIMESTAMP: a space separator normalizes the same way");

    /* The rule shortens a fraction. It must not do anything else. */
    expect("TIMESTAMP", "2026-01-31T14:22:05.100", "2026-01-31T14:22:05.001", 0,
           "TIMESTAMP: a fraction that differs is still a difference");
    expect("TIMESTAMP", "2026-01-31T14:22:05", "2026-01-31T14:22:06", 0,
           "TIMESTAMP: the seconds are untouched");
    expect("TIMESTAMP", "2026-01-31T14:22:05", "31/01/2026 14:22:05", 0,
           "TIMESTAMP: the date is still not parsed, per spec 5.3");
    expect("TIMESTAMP", "2026-01-31T14:22:05.000+05:30", "2026-01-31T14:22:05+05:30", 0,
           "TIMESTAMP: an offset suffix is declined rather than half normalized");
    expect("TIMESTAMP", "", "", 1, "TIMESTAMP: an empty value is still equal to itself");
    expect("TIMESTAMP", "2026-01-31T14:22:05.", "2026-01-31T14:22:05", 0,
           "TIMESTAMP: a bare trailing point is left alone");

    /* The canonical form belongs to the declared type, not to the text. */
    expect("VARCHAR(30)", "2026-01-31T14:22:05.000", "2026-01-31T14:22:05", 0,
           "VARCHAR: the same two values are text, and text differs");
    expect("DATE", "2026-01-31", "2026-01-31", 1, "DATE: unaffected, it carries no fraction");
}

static void test_quoting_is_still_invisible(void) {
    /* Phase 1's property has to survive normalization: the escaped path and the
     * bare path must land on the same normalized value and the same digest. */
    expect("VARCHAR(20)", "\"O\"\"Brien\"", "O\"Brien", 1,
           "an escaped quote still equals the bare value under a comparator");
    expect("DECIMAL(12,2)", "\"1.50\"", "1.5", 1, "and quoting does not stop numeric comparison");
}

/* ---------------------------------------------- the options and their stamp -- */

static void test_options(void) {
    ibha_csvd_compare_opts o;
    ibha_csvd_compare_opts_init(&o);
    o.trim_whitespace = 0;
    o.char_ignore_pad = 0;

    char sa[256], sb[256];
    one_col(sa, sizeof(sa), "VARCHAR(20)", " abc ");
    one_col(sb, sizeof(sb), "VARCHAR(20)", "abc");

    kit_pair p;
    int ok = kit_parse(&p, sa, sb, 4, &o) && kit_diff(&p, NULL) == 0;
    /* diff_run must refuse, because the tables were digested under settings the
     * diff was not given. A wrong answer here would be silent and confident. */
    TAP_OK(ok, "a diff whose comparison settings differ from the parse is refused");
    TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_ERR_INVALID_ARG, "with INVALID_ARG");
    kit_close(&p);

    ibha_csvd_diff_opts d;
    ibha_csvd_diff_opts_init(&d);
    d.compare = o;
    ok = kit_parse(&p, sa, sb, 4, &o) && kit_diff(&p, &d);
    TAP_OK(ok, "and accepted once the same settings are supplied to both");
    if (ok) {
        TAP_EQ_U64(ibha_csvd_diff_stats_of(p.diff)->rows_modified, 1,
                   "with trimming off, padding is a difference again");
    }
    kit_close(&p);

    /* An unimplemented option must be refused rather than quietly downgraded to
     * the default, which would look like it worked. */
    ibha_csvd_compare_opts dv;
    ibha_csvd_compare_opts_init(&dv);
    dv.date_compare = IBHA_CSVD_DATE_VALUE;
    kit_pair q;
    memset(&q, 0, sizeof(q));
    q.ctx = ibha_csvd_ctx_new(NULL);
    ibha_csvd_parse_opts po;
    kit_opts(&po, 4);
    po.compare = dv;
    TAP_OK(ibha_csvd_parse_begin(q.ctx, &po) == NULL, "date value comparison is refused, not faked");
    TAP_EQ_U64(ibha_csvd_ctx_status(q.ctx), IBHA_CSVD_ERR_UNIMPLEMENTED, "with UNIMPLEMENTED");
    kit_close(&q);
}

static void test_boolean_set_override(void) {
    ibha_csvd_compare_opts o;
    ibha_csvd_compare_opts_init(&o);
    o.bool_true = "OUI";
    o.bool_false = "NON";

    char sa[256], sb[256];
    one_col(sa, sizeof(sa), "BOOLEAN", "oui");
    one_col(sb, sizeof(sb), "BOOLEAN", "OUI");

    ibha_csvd_diff_opts d;
    ibha_csvd_diff_opts_init(&d);
    d.compare = o;

    kit_pair p;
    int ok = kit_parse(&p, sa, sb, 4, &o) && kit_diff(&p, &d);
    TAP_OK(ok && ibha_csvd_diff_stats_of(p.diff)->rows_unchanged == 1,
           "a caller supplied truth set is honoured");
    kit_close(&p);

    /* TRUE is no longer in the set, so it is text and must not be coerced. */
    one_col(sa, sizeof(sa), "BOOLEAN", "TRUE");
    one_col(sb, sizeof(sb), "BOOLEAN", "oui");
    ok = kit_parse(&p, sa, sb, 4, &o) && kit_diff(&p, &d);
    TAP_OK(ok && ibha_csvd_diff_stats_of(p.diff)->rows_modified == 1,
           "and a value outside it is not silently mapped into it");
    kit_close(&p);
}

void ibha_test_normalize(void) {
    test_type_parsing();
    test_decimal();
    test_integer();
    test_boolean();
    test_text_and_dates();
    test_timestamp_fraction();
    test_quoting_is_still_invisible();
    test_options();
    test_boolean_set_override();
}
