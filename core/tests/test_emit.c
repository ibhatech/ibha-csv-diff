/*
 * test_emit.c - the emitters of spec 13.3.
 *
 * The assertions divide into three kinds.
 *
 * **The contract.** The emitter and its consumer are separate components that
 * must agree, so the row shape is versioned and covered here: every JSONL line
 * carries schemaVersion, the CSV report carries it in a column, the HTML
 * container carries it in a data attribute, and the rule deciding when a cell
 * object carries "source" is asserted rather than left to be discovered by four
 * consumers independently.
 *
 * **The security property.** Cell content is untrusted and the HTML output is
 * injected through dangerouslySetInnerHTML. Every HTML assertion in this file
 * goes through ek_html_ok, which is a second implementation of what safe output
 * means, and fixtures/generated/xss.csv is run through it explicitly because that
 * fixture exists for exactly this moment: the parser carries the payload intact
 * so that the emitter is the thing that has to neutralize it.
 *
 * **The memory property.** An emitter is a loop over the cursor and nothing
 * accumulates, so emitting a report must not grow the arena.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diffkit.h"
#include "emitkit.h"
#include "suites.h"
#include "tap.h"

#define HE "KEY,,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),INTEGER\nid,name,qty\n"
#define OUT_CAP 65536

static char g_out[OUT_CAP];
static size_t g_len;

/* Emits into a fixed buffer and returns the status. g_out and g_len hold the
 * result; the sink counts past the end rather than truncating silently, so an
 * overflow is visible as g_len > OUT_CAP. */
static ibha_csvd_status emit_to(kit_pair *p, const ibha_csvd_emit_opts *o, uint32_t *rows) {
    ibha_csvd_buffer_sink bs;
    ibha_csvd_sink sink;

    memset(g_out, 0, sizeof(g_out));
    ibha_csvd_buffer_sink_init(&bs, g_out, sizeof(g_out) - 1);
    sink.write = ibha_csvd_buffer_sink_write;
    sink.ctx = &bs;

    ibha_csvd_status st = ibha_csvd_emit(p->diff, o, &sink, rows);
    g_len = bs.len < sizeof(g_out) - 1 ? bs.len : sizeof(g_out) - 1;
    return st;
}

static void opts_for(ibha_csvd_emit_opts *o, ibha_csvd_emit_format fmt) {
    ibha_csvd_emit_opts_init(o, fmt);
}

/* ------------------------------------------------------------------ jsonl -- */

static void test_jsonl(void) {
    kit_pair p;
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;

    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    if (kit_run(&p, HE "A,alice,1\nB,bob,2\nC,carol,3\n", HE "A,alicia,1\nC,carol,3\nD,dave,4\n",
                4)) {
        TAP_EQ_U64(emit_to(&p, &o, &rows), IBHA_CSVD_OK, "the jsonl emitter runs");
        int lines = ek_jsonl_ok(g_out, g_len);
        TAP_EQ_U64((uint64_t)lines, rows, "every line is one complete JSON value");
        TAP_EQ_U64(rows, ibha_csvd_diff_stats_of(p.diff)->report_rows,
                   "and there is one line per report row");
        TAP_OK(ek_contains(g_out, g_len, "\"schemaVersion\":1"),
               "every row carries the versioned contract");
        TAP_OK(ek_contains(g_out, g_len, "\"kind\":\"modified\"") &&
                   ek_contains(g_out, g_len, "\"kind\":\"added\"") &&
                   ek_contains(g_out, g_len, "\"kind\":\"deleted\"") &&
                   ek_contains(g_out, g_len, "\"kind\":\"unchanged\""),
               "and all four row kinds reach the stream");
    } else {
        for (int i = 0; i < 5; i++) TAP_OK(0, "the jsonl emitter runs");
    }
    kit_close(&p);

    /*
     * The cell contract. A matched row carries "source" exactly when the cell
     * differs in bytes from the target, so an unchanged 90,000 row report is not
     * written out twice over. One changed cell, one "source".
     */
    if (kit_run(&p, HE "A,alice,1\n", HE "A,alicia,1\n", 4)) {
        emit_to(&p, &o, &rows);
        int n_source = 0;
        for (size_t i = 0; i + 9 <= g_len; i++) {
            if (memcmp(g_out + i, "\"source\"", 8) == 0) n_source++;
        }
        TAP_EQ_U64((uint64_t)n_source, 1, "only the cell that differs carries a source value");
        TAP_OK(ek_contains(g_out, g_len, "\"source\":\"alice\",\"target\":\"alicia\",\"changed\":true"),
               "and it carries both sides and the changed flag");
    } else {
        TAP_OK(0, "only the cell that differs carries a source value");
        TAP_OK(0, "and it carries both sides and the changed flag");
    }
    kit_close(&p);

    /* Quoting is invisible to comparison, so it must be invisible to output: the
     * logical value is what is emitted, with JSON's own escaping applied. */
    if (kit_run(&p, HE "A,\"say \"\"hi\"\"\",1\n", HE "A,\"say \"\"bye\"\"\",1\n", 4)) {
        emit_to(&p, &o, &rows);
        TAP_OK(ek_jsonl_ok(g_out, g_len) > 0, "a value containing quotes stays valid JSON");
        TAP_OK(ek_contains(g_out, g_len, "say \\\"hi\\\""),
               "and the quotes are escaped rather than dropped");
    } else {
        TAP_OK(0, "a value containing quotes stays valid JSON");
        TAP_OK(0, "and the quotes are escaped rather than dropped");
    }
    kit_close(&p);

    /* A newline inside a quoted field must not become a line break in a format
     * whose whole premise is one row per line. */
    if (kit_run(&p, HE "A,\"one\ntwo\",1\n", HE "A,\"one\nthree\",1\n", 4)) {
        emit_to(&p, &o, &rows);
        TAP_EQ_U64((uint64_t)ek_jsonl_ok(g_out, g_len), 1,
                   "a multiline cell stays on one JSONL line");
    } else {
        TAP_OK(0, "a multiline cell stays on one JSONL line");
    }
    kit_close(&p);

    /* Findings ride on the row, which is the only way a consumer of the stream
     * ever learns about them. */
    if (kit_run(&p, HE "A,alice,1\n", HE "A,a name well past its twenty characters,x\n", 4)) {
        emit_to(&p, &o, &rows);
        TAP_OK(ek_contains(g_out, g_len, "\"kind\":\"tooLong\"") &&
                   ek_contains(g_out, g_len, "\"kind\":\"notNumeric\""),
               "validation findings reach the jsonl stream");
        TAP_OK(ek_contains(g_out, g_len, "\"limit\":20"), "with the limit that was breached");
    } else {
        TAP_OK(0, "validation findings reach the jsonl stream");
        TAP_OK(0, "with the limit that was breached");
    }
    kit_close(&p);
}

/* ----------------------------------------------------------------- shape -- */

static void test_shaping(void) {
    kit_pair p;
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;

    if (!kit_run(&p, HE "A,a,1\nB,b,2\nC,c,3\n", HE "A,a,1\nB,bb,2\nC,c,3\n", 4)) {
        for (int i = 0; i < 5; i++) TAP_OK(0, "the shaping fixture diffs");
        kit_close(&p);
        return;
    }

    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    o.changes_only = 1;
    emit_to(&p, &o, &rows);
    TAP_EQ_U64(rows, 1, "changes_only drops the rows that have nothing to say");

    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    o.max_rows = 2;
    emit_to(&p, &o, &rows);
    TAP_EQ_U64(rows, 2, "max_rows bounds the output, which is what HTML reports need");

    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    o.include_values = 0;
    emit_to(&p, &o, &rows);
    TAP_OK(!ek_contains(g_out, g_len, "\"cells\""),
           "include_values off emits the edit script alone");
    TAP_OK(ek_jsonl_ok(g_out, g_len) == 3, "and it is still one JSON value per row");

    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    o.max_cell_bytes = 1;
    emit_to(&p, &o, &rows);
    TAP_OK(ek_contains(g_out, g_len, "\"truncated\":true"),
           "a truncated value says so rather than silently losing bytes");
    kit_close(&p);

    /* changes_only keeps a row whose only news is a finding, because a finding on
     * an otherwise unchanged row is the point of running the comparison. */
    if (kit_run(&p, HE "A,a,1\n", HE "A,a,x\n", 4)) {
        opts_for(&o, IBHA_CSVD_EMIT_JSONL);
        o.changes_only = 1;
        emit_to(&p, &o, &rows);
        TAP_EQ_U64(rows, 1, "changes_only keeps a row that carries only a finding");
    } else {
        TAP_OK(0, "changes_only keeps a row that carries only a finding");
    }
    kit_close(&p);
}

/* ------------------------------------------------------------------- csv -- */

static size_t count_lines(const char *buf, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') n++;
    }
    return n;
}

static void test_csv(void) {
    kit_pair p;
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;

    opts_for(&o, IBHA_CSVD_EMIT_CSV);
    if (kit_run(&p, HE "A,a,1\nB,b,2\n", HE "A,aa,1\nC,c,3\n", 4)) {
        emit_to(&p, &o, &rows);
        /* One header, two lines for the modified row, one for the added row, one
         * for the deleted row. */
        TAP_EQ_U64(count_lines(g_out, g_len), 5, "a modified row is two CSV lines, old and new");
        TAP_OK(ek_contains(g_out, g_len, "schemaVersion,kind,side"),
               "the report names its own contract in the header");
        TAP_OK(ek_contains(g_out, g_len, "1,modified,source,") &&
                   ek_contains(g_out, g_len, "1,modified,target,"),
               "and each line says which side it came from");
    } else {
        for (int i = 0; i < 3; i++) TAP_OK(0, "the csv fixture diffs");
    }
    kit_close(&p);

    /* A value carrying the delimiter, a quote or a newline has to come back out
     * of a spreadsheet as the same value. */
    if (kit_run(&p, HE "A,x,1\n", HE "A,\"a,b \"\"c\"\"\",1\n", 4)) {
        emit_to(&p, &o, &rows);
        TAP_OK(ek_contains(g_out, g_len, "\"a,b \"\"c\"\"\""),
               "a value with a comma and a quote is quoted and its quotes doubled");
        TAP_EQ_U64(count_lines(g_out, g_len), 3, "and it stays on its own line");
    } else {
        TAP_OK(0, "a value with a comma and a quote is quoted and its quotes doubled");
        TAP_OK(0, "and it stays on its own line");
    }
    kit_close(&p);

    /*
     * Formula injection. A report of untrusted data opened in Excel is a script
     * delivery mechanism unless the leading '=' is neutralized, and an ordinary
     * negative number must not be mangled in the process.
     */
    if (kit_run(&p, HE "A,x,1\nB,y,2\nC,z,3\n", HE "A,\"=cmd|' /c calc'!A1\",1\nB,-5,2\nC,-1+1,3\n",
                4)) {
        emit_to(&p, &o, &rows);
        TAP_OK(ek_contains(g_out, g_len, "\"'=cmd"), "a formula is prefixed with Excel's own guard");
        TAP_OK(ek_contains(g_out, g_len, ",-5,"), "an ordinary negative number is left alone");
        TAP_OK(ek_contains(g_out, g_len, "\"'-1+1\""), "and a formula wearing a minus sign is not");

        opts_for(&o, IBHA_CSVD_EMIT_CSV);
        o.csv_formula_guard = 0;
        emit_to(&p, &o, &rows);
        TAP_OK(!ek_contains(g_out, g_len, "\"'=cmd"), "the guard can be turned off");
    } else {
        for (int i = 0; i < 4; i++) TAP_OK(0, "the formula guard fixture diffs");
    }
    kit_close(&p);
}

/* ------------------------------------------------------------------ html -- */

static void test_html(void) {
    kit_pair p;
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;

    opts_for(&o, IBHA_CSVD_EMIT_HTML);
    if (kit_run(&p, HE "A,alice,1\nB,bob,2\n", HE "A,alicia,1\nC,carol,3\n", 4)) {
        TAP_EQ_U64(emit_to(&p, &o, &rows), IBHA_CSVD_OK, "the html emitter runs");
        TAP_OK(ek_html_ok(g_out, g_len), "and every tag it writes is one it is allowed to write");
        TAP_OK(ek_contains(g_out, g_len, "data-schema-version=\"1\""),
               "the container carries the versioned contract");
        TAP_OK(ek_contains(g_out, g_len, "class=\"ibha-csvd-row ibha-csvd-modified\"") &&
                   ek_contains(g_out, g_len, "ibha-csvd-added") &&
                   ek_contains(g_out, g_len, "ibha-csvd-deleted"),
               "and the row classes carry the documented prefix");
    } else {
        for (int i = 0; i < 4; i++) TAP_OK(0, "the html emitter runs");
    }
    kit_close(&p);

    /* The class prefix is the one caller supplied string that reaches the markup,
     * so it is validated rather than escaped. */
    if (kit_run(&p, HE "A,a,1\n", HE "A,b,1\n", 4)) {
        opts_for(&o, IBHA_CSVD_EMIT_HTML);
        o.class_prefix = "acme-";
        TAP_EQ_U64(emit_to(&p, &o, &rows), IBHA_CSVD_OK, "a valid class prefix is accepted");
        TAP_OK(ek_contains(g_out, g_len, "class=\"acme-report\""), "and is used");

        kit_close(&p);
        (void)kit_run(&p, HE "A,a,1\n", HE "A,b,1\n", 4);
        opts_for(&o, IBHA_CSVD_EMIT_HTML);
        o.class_prefix = "x\" onload=\"alert(1)";
        TAP_EQ_U64(emit_to(&p, &o, &rows), IBHA_CSVD_ERR_INVALID_ARG,
                   "a class prefix that is not an identifier is refused, not escaped");
    } else {
        for (int i = 0; i < 3; i++) TAP_OK(0, "the class prefix fixture diffs");
    }
    kit_close(&p);

    /* Intra cell highlighting, spec 7, inside the emitter that consumes it. */
    if (kit_run(&p, HE "A,\"Accident violation code\",1\n",
                HE "A,\"Accident Violation code(s)\",1\n", 4)) {
        opts_for(&o, IBHA_CSVD_EMIT_HTML);
        o.cell_diff = IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER;
        emit_to(&p, &o, &rows);
        TAP_OK(ek_html_ok(g_out, g_len), "cell level highlighting keeps the output safe");
        TAP_OK(ek_contains(g_out, g_len, "<ins class=\"ibha-csvd-ins\">"),
               "and marks what was inserted inside the cell");
        TAP_OK(ek_contains(g_out, g_len, "Accident "),
               "while the unchanged part of the cell stays outside the marks");
    } else {
        for (int i = 0; i < 3; i++) TAP_OK(0, "the cell diff fixture diffs");
    }
    kit_close(&p);
}

/* -------------------------------------------------------------- the XSS -- */

static uint8_t *read_fixture(const char *dir, const char *name, size_t *out_len) {
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
    buf[got] = 0;
    *out_len = got;
    return buf;
}

/*
 * fixtures/generated/xss.csv carries <script>alert(1)</script>, a value that is
 * literally `" onload="x`, and a javascript: URL. The parser is required to carry
 * them through intact, which means this emitter is the only thing standing
 * between them and the approver's browser.
 */
static void test_xss(const char *dir) {
    size_t len = 0;
    uint8_t *body = read_fixture(dir, "xss.csv", &len);
    if (!body) {
        TAP_OK(0, "fixtures/generated/xss.csv is present");
        printf("  missing fixture, run: make fixtures\n");
        return;
    }
    TAP_OK(len > 0, "fixtures/generated/xss.csv is present");

    /* The payload is compared against a benign row, so it arrives as an added
     * row, a deleted row and a modified cell in one pass. */
    static const char k_clean[] = "account_id,period,customer_name,region,premium_amount,currency,"
                                  "status,term_months,notes,is_active,commission_rate,"
                                  "effective_date\nSAFE,2026-01,ok,NORTH,1.00,USD,ACTIVE,6,n,TRUE,"
                                  "0.010,2026-01-15\n";
    kit_pair p;
    memset(&p, 0, sizeof(p));
    p.ctx = ibha_csvd_ctx_new(NULL);

    ibha_csvd_parse_opts so;
    kit_opts(&so, 1);
    p.src = ibha_csvd_parse_begin(p.ctx, &so);
    ibha_csvd_parse_borrow(p.src, k_clean, sizeof(k_clean) - 1);
    ibha_csvd_parse_finish(p.src);

    ibha_csvd_parse_opts to;
    kit_opts(&to, 1);
    to.header.rows = IBHA_CSVD_HEADER_AUTO;
    to.expect_table = ibha_csvd_table_of(p.src);
    to.expect_schema = ibha_csvd_schema_of(p.src);
    p.tgt = ibha_csvd_parse_begin(p.ctx, &to);
    ibha_csvd_parse_borrow(p.tgt, body, len);
    ibha_csvd_parse_finish(p.tgt);

    if (!kit_diff(&p, NULL)) {
        TAP_OK(0, "the xss fixture diffs against a clean row");
        for (int i = 0; i < 8; i++) TAP_OK(0, "xss assertion");
        kit_close(&p);
        free(body);
        return;
    }
    TAP_OK(1, "the xss fixture diffs against a clean row");

    ibha_csvd_emit_opts o;
    uint32_t rows = 0;
    opts_for(&o, IBHA_CSVD_EMIT_HTML);
    emit_to(&p, &o, &rows);

    TAP_OK(ek_html_ok(g_out, g_len), "the xss payload produces no tag the emitter did not write");
    TAP_OK(!ek_contains(g_out, g_len, "<script"), "the script tag does not survive as markup");
    TAP_OK(ek_contains(g_out, g_len, "&lt;script&gt;"), "it survives as text, which is the point");
    TAP_OK(!ek_contains(g_out, g_len, " onload=\""),
           "and the attribute injection does not close its own quote");
    TAP_OK(ek_contains(g_out, g_len, "&quot; onload=&quot;x"),
           "both quote characters are escaped, not only the tag delimiters");

    /* The same payload again with intra cell highlighting on, which cuts the
     * value into pieces and writes markup between them. That is the path where an
     * escaper is easiest to get wrong, so it gets the invariant too. */
    opts_for(&o, IBHA_CSVD_EMIT_HTML);
    o.cell_diff = IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER;
    emit_to(&p, &o, &rows);
    TAP_OK(ek_html_ok(g_out, g_len), "and no tag survives the intra cell highlighting path either");
    TAP_OK(!ek_contains(g_out, g_len, "<script") && !ek_contains(g_out, g_len, " onload=\""),
           "with the payload still cut into escaped pieces rather than markup");

    /* The same payload through the JSONL emitter has to stay one line and stay
     * parseable, because a consumer splitting on newlines is the normal case. */
    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    emit_to(&p, &o, &rows);
    TAP_OK(ek_jsonl_ok(g_out, g_len) == (int)rows, "and the same payload is valid JSONL");

    kit_close(&p);
    free(body);
}

/*
 * The overlong encoding of '<' is 0xC0 0xBC. It is not the byte 0x3C, so an
 * escaper that works a byte at a time passes it through untouched, and a decoder
 * that accepts overlong forms then sees a tag. Rejecting every ill formed
 * sequence is what closes that off, and this is the assertion that says so.
 */
static void test_overlong(void) {
    kit_pair p;
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;

    if (kit_run(&p, HE "A,safe,1\n", HE "A,\xC0\xBCscript\xC0\xBE,1\n", 4)) {
        opts_for(&o, IBHA_CSVD_EMIT_HTML);
        emit_to(&p, &o, &rows);
        TAP_OK(!ek_contains(g_out, g_len, "\xC0\xBC"),
               "an overlong UTF-8 encoding of '<' is not passed through to HTML");
        TAP_OK(ek_contains(g_out, g_len, "\xEF\xBF\xBD"), "it becomes a replacement character");
        TAP_OK(ek_html_ok(g_out, g_len), "and the output is still well formed");

        opts_for(&o, IBHA_CSVD_EMIT_JSONL);
        emit_to(&p, &o, &rows);
        TAP_OK(ek_jsonl_ok(g_out, g_len) > 0, "and the JSONL line still parses");
        TAP_OK(ek_contains(g_out, g_len, "\"invalidUtf8\":true"),
               "with the value marked as having been repaired");
    } else {
        for (int i = 0; i < 5; i++) TAP_OK(0, "the overlong fixture diffs");
    }
    kit_close(&p);

    /* A Latin-1 file is not valid UTF-8 and must not be rejected: one byte
     * becomes one replacement character and the bytes after it survive. */
    if (kit_run(&p, HE "A,safe,1\n", HE "A,caf\xE9 ltd,1\n", 4)) {
        opts_for(&o, IBHA_CSVD_EMIT_JSONL);
        emit_to(&p, &o, &rows);
        TAP_OK(ek_jsonl_ok(g_out, g_len) > 0, "a Latin-1 value still produces valid JSON");
        TAP_OK(ek_contains(g_out, g_len, "\\ufffd ltd"),
               "and one bad byte costs one replacement, not the rest of the value");

        /* CSV has no encoding contract and carries no markup, so it is byte
         * transparent and the original byte comes back out. */
        opts_for(&o, IBHA_CSVD_EMIT_CSV);
        emit_to(&p, &o, &rows);
        TAP_OK(ek_contains(g_out, g_len, "caf\xE9 ltd"), "while the CSV report keeps the bytes");
    } else {
        for (int i = 0; i < 3; i++) TAP_OK(0, "the latin-1 fixture diffs");
    }
    kit_close(&p);
}

/* --------------------------------------------------------------- summary -- */

static void test_summary(void) {
    kit_pair p;
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;

    opts_for(&o, IBHA_CSVD_EMIT_SUMMARY);
    if (kit_run(&p, HE "A,a,1\nB,b,2\n", HE "A,aa,1\nC,c,3\n", 4)) {
        TAP_EQ_U64(emit_to(&p, &o, &rows), IBHA_CSVD_OK, "the summary emitter runs");
        TAP_OK(ek_jsonl_ok(g_out, g_len) == 1, "and produces one JSON object");
        TAP_OK(ek_contains(g_out, g_len, "\"modified\":1") &&
                   ek_contains(g_out, g_len, "\"added\":1") &&
                   ek_contains(g_out, g_len, "\"deleted\":1"),
               "carrying the row counts");
        TAP_OK(ek_contains(g_out, g_len, "\"identical\":false"),
               "and a pass/fail answer that needs no parsing of the counts");

        /*
         * The counters accumulate as a cursor advances, so a caller who already
         * wrote JSONL would see them doubled. The summary is defined as the
         * numbers of exactly one pass, so emitting it twice must not drift.
         */
        emit_to(&p, &o, &rows);
        size_t first_len = g_len;
        char keep[OUT_CAP];
        memcpy(keep, g_out, first_len);
        emit_to(&p, &o, &rows);
        TAP_OK(g_len == first_len && memcmp(keep, g_out, first_len) == 0,
               "and emitting it twice produces the same numbers");
    } else {
        for (int i = 0; i < 5; i++) TAP_OK(0, "the summary fixture diffs");
    }
    kit_close(&p);

    if (kit_run(&p, HE "A,a,1\n", HE "A,a,1\n", 4)) {
        emit_to(&p, &o, &rows);
        TAP_OK(ek_contains(g_out, g_len, "\"identical\":true"),
               "two identical files summarize as identical");
    } else {
        TAP_OK(0, "two identical files summarize as identical");
    }
    kit_close(&p);

    /* Spec 13.8: where the uploaded file's own metadata disagrees with the
     * source's, the source wins and the disagreement is a finding, not an error. */
    if (kit_run(&p, HE "A,a,1\n", "KEY,,\nREQUIRED,,\nVARCHAR(10),VARCHAR(20),VARCHAR(9)\nid,name,qty\nA,a,1\n",
                4)) {
        emit_to(&p, &o, &rows);
        TAP_OK(ek_contains(g_out, g_len, "\"kind\":\"metadataDisagreement\""),
               "a target that disagrees about a declared type is a finding");
        TAP_EQ_U64(ibha_csvd_ctx_status(p.ctx), IBHA_CSVD_OK, "and not an error");
    } else {
        TAP_OK(0, "a target that disagrees about a declared type is a finding");
        TAP_OK(0, "and not an error");
    }
    kit_close(&p);
}

/* ----------------------------------------------------------- the plumbing -- */

static int refuse_write(void *ctx, const void *bytes, size_t len) {
    (void)bytes;
    (void)len;
    (*(int *)ctx)++;
    return -1;
}

static void test_plumbing(void) {
    kit_pair p;
    ibha_csvd_emit_opts o;
    uint32_t rows = 0;

    if (!kit_run(&p, HE "A,a,1\nB,b,2\n", HE "A,aa,1\nC,c,3\n", 4)) {
        for (int i = 0; i < 6; i++) TAP_OK(0, "the plumbing fixture diffs");
        kit_close(&p);
        return;
    }

    /* A sink that refuses is an IO error, and it stops the walk rather than
     * being written past. */
    int calls = 0;
    ibha_csvd_sink bad;
    bad.write = refuse_write;
    bad.ctx = &calls;
    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    TAP_EQ_U64(ibha_csvd_emit(p.diff, &o, &bad, &rows), IBHA_CSVD_ERR_IO,
               "a sink that refuses a write surfaces as an IO error");
    TAP_EQ_U64(calls, 1, "and is not called again after it refused");
    kit_close(&p);

    if (!kit_run(&p, HE "A,a,1\nB,b,2\n", HE "A,aa,1\nC,c,3\n", 4)) {
        for (int i = 0; i < 4; i++) TAP_OK(0, "the plumbing fixture diffs");
        kit_close(&p);
        return;
    }

    /* The buffer sink counts past the end, so a caller can size a buffer from a
     * first pass instead of bisecting for the answer. */
    char tiny[8];
    ibha_csvd_buffer_sink bs;
    ibha_csvd_sink sink;
    ibha_csvd_buffer_sink_init(&bs, tiny, sizeof(tiny));
    sink.write = ibha_csvd_buffer_sink_write;
    sink.ctx = &bs;
    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    ibha_csvd_emit(p.diff, &o, &sink, &rows);
    TAP_OK(bs.overflow && bs.len > sizeof(tiny),
           "an undersized buffer sink reports the length it would have needed");

    /* Nothing accumulates. Emitting a whole report is a loop over the cursor into
     * the sink, so the arena must not grow while it runs. */
    ibha_csvd_buffer_sink bs2;
    ibha_csvd_buffer_sink_init(&bs2, g_out, sizeof(g_out));
    sink.ctx = &bs2;
    ibha_csvd_emit(p.diff, &o, &sink, &rows); /* first call opens a cursor */
    uint64_t before = ibha_csvd_ctx_bytes_reserved(p.ctx);
    for (int i = 0; i < 20; i++) {
        ibha_csvd_buffer_sink_init(&bs2, g_out, sizeof(g_out));
        ibha_csvd_emit(p.diff, &o, &sink, &rows);
    }
    uint64_t after = ibha_csvd_ctx_bytes_reserved(p.ctx);
    TAP_OK(after - before < 64 * 1024, "twenty more passes cost at most one arena block");

    opts_for(&o, IBHA_CSVD_EMIT_JSONL);
    o.format = 99;
    TAP_EQ_U64(ibha_csvd_emit(p.diff, &o, &sink, &rows), IBHA_CSVD_ERR_INVALID_ARG,
               "an unknown format is refused");
    kit_close(&p);
}

void ibha_test_emit(const char *fixture_dir) {
    test_jsonl();
    test_shaping();
    test_csv();
    test_html();
    test_xss(fixture_dir);
    test_overlong();
    test_summary();
    test_plumbing();
}
