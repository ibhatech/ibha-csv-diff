package com.ibhatech.csvdiff;

import java.nio.charset.StandardCharsets;
import java.sql.ResultSet;
import java.sql.Timestamp;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The five flagged assumptions, made visible so a person can confirm or reject them.
 *
 * <p>Group B of {@code specs/03-remaining-tasks.md}. Each of the five is implemented,
 * unconfirmed, and fails silently: the diff returns a wrong answer that looks like a
 * right answer. Six phases of "flag it, do not re-decide it" was correct during
 * construction and is not correct any more.
 *
 * <p><strong>This tool does not decide anything.</strong> It runs the real engine
 * through the real binding on the smallest input that makes each rule observable, and
 * prints what actually happened next to what the alternative would have produced. The
 * answer to each is a judgement about the data a deployment really holds, which is
 * not in this repository. Driven by
 * {@code scripts-and-commands/confirm_assumptions.py}.
 *
 * <p>In this package rather than in {@code tools} because B1 and B2 need
 * {@link FakeResultSet} and {@link SqlValues}, both package private, and because a
 * real driver would test someone else's type mapping rather than the rule under
 * examination.
 */
public final class AssumptionsMain {

    private AssumptionsMain() {
    }

    private static final String RULE = "-".repeat(78);

    public static void main(String[] args) {
        List<String> want = args.length == 0
                ? List.of("b1", "b2", "b3", "b4", "b5")
                : Arrays.asList(args);

        for (String one : want) {
            switch (one.toLowerCase(java.util.Locale.ROOT)) {
                case "b1" -> b1SqlNull();
                case "b2" -> b2TimestampFormat();
                case "b3" -> b3RaggedRows();
                case "b4" -> b4DuplicateKeyRowNumbers();
                case "b5" -> b5VarcharCounting();
                default -> throw new IllegalArgumentException("unknown assumption " + one);
            }
        }
    }

    // ------------------------------------------------------------------ B1 --

    /**
     * SQL NULL renders as an empty field, indistinguishable from an empty string.
     * The question is what the CSV side of the comparison writes for a null.
     */
    private static void b1SqlNull() {
        heading("B1", "SQL NULL renders as an empty field");

        Header header = Header.builder()
                .column("id", "INTEGER").key().required()
                .column("note", "VARCHAR(40)")
                .build();

        // One row whose second column is SQL NULL.
        List<List<Object>> rows = List.of(Arrays.asList(1, null));

        System.out.println("The JDBC side is one row, id=1, note=SQL NULL.");
        System.out.println("SqlValues renders that null as: " + quoted(renderNull()));
        System.out.println();
        System.out.println("Compared against a CSV export that writes a null as ...");
        System.out.println();

        for (String sentinel : List.of("", "\\N", "NULL", "(null)")) {
            String csv = headerCsv(header) + "1," + sentinel + "\n";
            ResultSet rs = FakeResultSet.of(2, rows);
            String verdict = compareVerdict(
                    DiffSource.ofResultSet(rs, header),
                    DiffSource.ofBytes(csv.getBytes(StandardCharsets.UTF_8)));
            System.out.printf("  export writes %-8s ->  %s%n", quoted(sentinel), verdict);
        }

        System.out.println();
        System.out.println("""
                What this means. Only the empty field agrees. If the export that feeds
                the other side of a real reconciliation writes any sentinel at all, then
                every null cell in the table reports as a changed value, and the report
                is noise rather than a finding.

                THE QUESTION: what does your CSV export actually write for a NULL?
                If it is an empty field, this assumption is confirmed and closes.
                If it is a sentinel, SqlValues needs a nullText option to match it.""");
    }

    private static String renderNull() {
        try {
            // Cast, or the ResultSet overload is the more specific match and wins.
            return SqlValues.render((Object) null, 1);
        } catch (java.sql.SQLException e) {
            throw new IllegalStateException(e);
        }
    }

    // ------------------------------------------------------------------ B2 --

    /**
     * Timestamps render as {@code 2026-01-31T14:22:05}. The question is whether the
     * CSV side carries the same text, since the engine compares dates as text.
     */
    private static void b2TimestampFormat() {
        heading("B2", "The timestamp and date text format");

        System.out.println("What SqlValues renders, for the values a driver hands over:");
        System.out.println();
        show("TIMESTAMP 2026-01-31 14:22:05.000", Timestamp.valueOf("2026-01-31 14:22:05"));
        show("TIMESTAMP 2026-01-31 14:22:05.100", Timestamp.valueOf("2026-01-31 14:22:05.1"));
        show("TIMESTAMP 2026-01-31 00:00:00.000", Timestamp.valueOf("2026-01-31 00:00:00"));
        show("DATE      2026-01-31", java.sql.Date.valueOf("2026-01-31"));
        show("TIME      14:22:05", java.sql.Time.valueOf("14:22:05"));
        System.out.println();

        Header header = Header.builder()
                .column("id", "INTEGER").key().required()
                .column("updated", "TIMESTAMP")
                .build();
        List<List<Object>> rows =
                List.of(List.of(1, Timestamp.valueOf("2026-01-31 14:22:05")));

        System.out.println("A JDBC TIMESTAMP compared against a CSV carrying ...");
        System.out.println();

        List<String> forms = List.of(
                "2026-01-31T14:22:05",
                "2026-01-31 14:22:05",
                "31/01/2026 14:22:05",
                "2026-01-31T14:22:05.000",
                "01/31/2026 02:22:05 PM");

        for (String form : forms) {
            String csv = headerCsv(header) + "1," + form + "\n";
            ResultSet rs = FakeResultSet.of(2, rows);
            String verdict = compareVerdict(
                    DiffSource.ofResultSet(rs, header),
                    DiffSource.ofBytes(csv.getBytes(StandardCharsets.UTF_8)));
            System.out.printf("  %-24s ->  %s%n", quoted(form), verdict);
        }

        System.out.println();
        System.out.println("""
                What this means. Only the ISO form with the T agrees. A timestamp column
                is usually every row of the table, so a format disagreement here does not
                report a few changed cells, it reports the whole table as modified.

                Note the engine compares these as TEXT. Comparing them by value is
                IBHA_CSVD_DATE_VALUE in the engine and is explicitly not implemented, so
                matching a different format is binding work, and comparing by value is
                engine work.

                THE QUESTION: what format do the CSVs you compare against carry?
                If it is the ISO T form, this closes. If not, the fix is a few lines in
                SqlValues, and the choice of format has to be made once and stated.""");
    }

    private static void show(String label, Object v) {
        try {
            System.out.printf("  %-34s ->  %s%n", label, quoted(SqlValues.render(v, 1)));
        } catch (java.sql.SQLException e) {
            throw new IllegalStateException(e);
        }
    }

    // ------------------------------------------------------------------ B3 --

    /**
     * Extra trailing empty fields are normalized; missing fields are an error. The
     * asymmetry is the thing to confirm.
     */
    private static void b3RaggedRows() {
        heading("B3", "The asymmetric ragged row rule");

        String head = """
                KEY,,
                REQUIRED,,
                INTEGER,VARCHAR(20),VARCHAR(20)
                id,name,region
                """;

        System.out.println("A three column file. Each case is one data row of a different width.");
        System.out.println();

        record Case(String what, String row) {
        }
        List<Case> cases = List.of(
                new Case("exactly three fields", "1,ann,NE"),
                new Case("four fields, the extra one EMPTY", "1,ann,NE,"),
                new Case("five fields, both extras EMPTY", "1,ann,NE,,"),
                new Case("four fields, the extra one NOT empty", "1,ann,NE,x"),
                new Case("two fields, one MISSING", "1,ann"));

        for (Case c : cases) {
            String csv = head + c.row() + "\n";
            String verdict = compareVerdict(
                    DiffSource.ofBytes(csv.getBytes(StandardCharsets.UTF_8)),
                    DiffSource.ofBytes(csv.getBytes(StandardCharsets.UTF_8)));
            System.out.printf("  %-36s %-14s ->  %s%n", c.what(), quoted(c.row()), verdict);
        }

        System.out.println();
        System.out.println("""
                What this means. Excess empty fields are silently normalized and counted
                in stats.ragged_normalized, because Excel emits trailing empty columns
                routinely and rejecting those files would be a support burden. A row with
                too few fields is refused, because missing data means the file is broken
                and padding it would hide the corruption a diff exists to surface.

                The asymmetry is deliberate. It is also the part nobody has confirmed.

                THE QUESTION: is a short row really an error for your data? A system that
                omits trailing empty columns rather than padding them would produce short
                rows legitimately, and every such file would be refused outright. The fix
                is three lines in row_end in core/src/parse.c.""");
    }

    // ------------------------------------------------------------------ B4 --

    /**
     * Row numbers in every message and every row are 1 based record numbers, which
     * differ from physical line numbers whenever a field spans lines.
     */
    private static void b4DuplicateKeyRowNumbers() {
        heading("B4", "Duplicate key row numbers are record based, not line based");

        // A multiline quoted field pushes the physical lines out of step with the
        // records, which is the only situation in which the two can differ.
        String csv = """
                KEY,
                REQUIRED,
                INTEGER,VARCHAR(40)
                id,note
                1,"first line
                second line"
                2,plain
                1,"duplicate of the first key"
                """;

        System.out.println("The file, with physical line numbers on the left:");
        System.out.println();
        String[] lines = csv.split("\n", -1);
        for (int i = 0; i < lines.length; i++) {
            if (i == lines.length - 1 && lines[i].isEmpty()) break;
            System.out.printf("    line %d | %s%n", i + 1, lines[i]);
        }

        System.out.println();
        System.out.println("""
                    Four header rows, then three data records. The first data record
                    spans two physical lines, so from there on the two numberings differ.

                    record 5 = lines 5 and 6   id=1
                    record 6 = line  7         id=2
                    record 7 = line  8         id=1   <- the duplicate""");
        System.out.println();

        String verdict = compareVerdict(
                DiffSource.ofBytes(csv.getBytes(StandardCharsets.UTF_8)),
                DiffSource.ofBytes(csv.getBytes(StandardCharsets.UTF_8)));
        System.out.println("  what the engine reports:");
        System.out.println("    " + verdict);

        System.out.println();
        System.out.println("""
                What this means. The numbers name records, counting every parsed record
                including the header rows, not physical lines. For a file with no blank
                lines and no multiline fields the two are identical, which is most files,
                which is why this can sit unnoticed until the one file where it matters.

                The same numbering reaches DiffRow.sourceRow and targetRow and every
                emitter, so it is not only the error message.

                THE QUESTION: when someone opens the file in an editor to find the row
                this names, do they land on the right one? If a person expects a line
                number, this is a support call every time a file has a multiline field.""");
    }

    // ------------------------------------------------------------------ B5 --

    /**
     * VARCHAR(n) is measured in characters, not bytes. The question is how the
     * source database measures it.
     */
    private static void b5VarcharCounting() {
        heading("B5", "VARCHAR(n) counts characters, not bytes");

        String head = """
                KEY,
                REQUIRED,
                INTEGER,VARCHAR(5)
                id,name
                """;

        System.out.println("Column 2 is VARCHAR(5). Each case is one value.");
        System.out.println();

        for (String value : List.of("abcde", "abcdef", "cafe", "café", "cafés",
                "ééééé", "éééééé")) {
            String csv = head + "1," + value + "\n";
            int chars = value.codePointCount(0, value.length());
            int bytes = value.getBytes(StandardCharsets.UTF_8).length;
            String verdict = findingVerdict(csv);
            System.out.printf("  %-8s %d chars, %d bytes  ->  %s%n",
                    quoted(value), chars, bytes, verdict);
        }

        System.out.println();
        System.out.println("""
                What this means. The rule is one line in char_count in
                core/src/validate.c: it counts UTF-8 lead bytes, so it counts characters.
                A value of 5 characters in 6 or 10 bytes passes VARCHAR(5).

                Databases disagree about this and both answers are common. Oracle's
                default is BYTE semantics unless the column was declared CHAR semantics
                or NLS_LENGTH_SEMANTICS says otherwise; SQL Server NVARCHAR(n) counts
                characters; PostgreSQL varchar(n) counts characters.

                It matters exactly when the data has non-ASCII text in a column that is
                near its declared limit, which is names and addresses, which is the data
                people reconcile.

                THE QUESTION: how does the database this schema came from count?
                If it counts bytes and this counts characters, the diff will pass a value
                that the target system will then refuse on insert, which is the failure
                that costs the most to diagnose, because the report said it was fine.""");
    }

    // -------------------------------------------------------------- helpers --

    /** The four header rows a Header writes, as CSV text. */
    private static String headerCsv(Header header) {
        StringBuilder sb = new StringBuilder();
        for (List<String> row : header.toRows()) {
            sb.append(String.join(",", row)).append('\n');
        }
        return sb.toString();
    }

    /**
     * Runs one comparison and reduces it to a line a person can read. An error is a
     * verdict too, and is the expected outcome in several of these cases.
     */
    private static String compareVerdict(DiffSource source, DiffSource target) {
        try (Diff diff = CsvDiff.compare(source, target)) {
            DiffSummary s = diff.summary();
            if (s.identical()) {
                return "identical";
            }
            List<String> parts = new ArrayList<>();
            if (s.rows().modified() > 0) parts.add(s.rows().modified() + " modified");
            if (s.rows().added() > 0) parts.add(s.rows().added() + " added");
            if (s.rows().deleted() > 0) parts.add(s.rows().deleted() + " deleted");
            if (s.cells().changed() > 0) parts.add(s.cells().changed() + " changed cells");
            if (s.findings().total() > 0) parts.add(s.findings().total() + " findings");
            return parts.isEmpty() ? "not identical, no row or cell difference"
                    : String.join(", ", parts);
        } catch (CsvDiffException e) {
            return "REFUSED: " + e.getMessage();
        }
    }

    /** Compares a file against itself, so the only thing reported is validation. */
    private static String findingVerdict(String csv) {
        byte[] bytes = csv.getBytes(StandardCharsets.UTF_8);
        try (Diff diff = CsvDiff.compare(DiffSource.ofBytes(bytes), DiffSource.ofBytes(bytes))) {
            List<Finding> found = new ArrayList<>();
            diff.rows(RowOptions.defaults()).forEach(r -> found.addAll(r.findings()));
            if (found.isEmpty()) return "no finding, accepted";
            StringBuilder sb = new StringBuilder();
            for (Finding f : found) {
                if (sb.length() > 0) sb.append("; ");
                sb.append(f.kind());
            }
            return sb.toString();
        } catch (CsvDiffException e) {
            return "REFUSED: " + e.getMessage();
        }
    }

    private static String quoted(String s) {
        return "\"" + s + "\"";
    }

    private static void heading(String id, String title) {
        System.out.println();
        System.out.println(RULE);
        System.out.println(id + ". " + title);
        System.out.println(RULE);
        System.out.println();
    }
}
