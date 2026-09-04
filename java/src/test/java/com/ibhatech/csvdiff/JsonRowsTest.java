package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayInputStream;
import java.io.Reader;
import java.io.StringReader;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.sql.Clob;
import java.sql.ResultSet;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The JSON row source: an array of objects in a column, compared as a table.
 *
 * <p>Use case 3 of {@code specs/input-options.md}, and the last item of the
 * ingestion track. The three decisions it was blocked on were answered: array of
 * objects only, a key that does not line up with the header is an error, and values
 * render through {@link SqlValues} rather than through a second renderer.
 *
 * <p>That last one is why several of these compare a JSON side against a
 * <em>JDBC</em> side of the same data rather than asserting a rendering in
 * isolation. The rule that matters is not "null renders as an empty field", it is
 * "the JSON side and the JDBC side of one row agree", and only a comparison of the
 * two can fail when they stop agreeing.
 */
class JsonRowsTest {

    private static final List<String> NAMES = List.of("id", "name", "premium");

    private static Header keyed() {
        return Header.builder()
                .column("id", "INTEGER").key().required()
                .column("name", "VARCHAR(20)")
                .column("premium", "DECIMAL(12,2)")
                .build();
    }

    private static final String THREE_ROWS = """
            [{"id":"1","name":"Ann","premium":"10.50"},
             {"id":"2","name":"Bo","premium":"20.00"},
             {"id":"3","name":"Cy","premium":"30.25"}]
            """;

    private static DiffSource json(String text) {
        return DiffSource.ofJsonRows(keyed(), new StringReader(text));
    }

    /* ------------------------------------------------ the same as the rows -- */

    /** The load bearing equivalence: a JSON array and the same rows through the
     *  ordinary row feed are the same table. */
    @Test
    void aJsonArrayReportsWhatTheSameRowsDo() {
        List<List<String>> rows = List.of(
                List.of("1", "Ann", "10.50"),
                List.of("2", "Bo", "20.00"),
                List.of("3", "Cy", "30.25"));
        List<List<String>> changed = new ArrayList<>(rows);
        changed.set(1, List.of("2", "Bo", "22.00"));

        String targetJson = """
                [{"id":"1","name":"Ann","premium":"10.50"},
                 {"id":"2","name":"Bo","premium":"22.00"},
                 {"id":"3","name":"Cy","premium":"30.25"}]
                """;

        List<String> viaRows;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofRows(keyed(), rows.iterator()),
                DiffSource.ofRows(keyed(), changed.iterator()))) {
            viaRows = describe(d);
        }

        List<String> viaJson;
        try (Diff d = CsvDiff.compare(json(THREE_ROWS), json(targetJson))) {
            viaJson = describe(d);
        }

        assertEquals(viaRows, viaJson);
        assertTrue(viaJson.stream().anyMatch(r -> r.contains("20.00|22.00")), viaJson.toString());
    }

    /** A JSON original against an uploaded CSV, which is what use case 3 turns into
     *  once one side is a file. */
    @Test
    void aJsonSideComparesAgainstACsvFile() {
        byte[] csv = csv(keyed(), List.of(
                List.of("1", "Ann", "10.50"),
                List.of("2", "Bo", "20.00"),
                List.of("3", "Cy", "30.25")));
        try (Diff d = CsvDiff.compare(json(THREE_ROWS), DiffSource.ofBytes(csv))) {
            assertEquals(NAMES, d.columns());
            assertTrue(d.summary().identical(), describe(d).toString());
        }
    }

    /** JSON objects are unordered by definition, and two rows of one export can list
     *  their keys differently. The header decides the column order, not the file. */
    @Test
    void theHeaderDecidesTheColumnOrderNotTheKeyOrder() {
        String shuffled = """
                [{"premium":"10.50","id":"1","name":"Ann"},
                 {"name":"Bo","premium":"20.00","id":"2"},
                 {"id":"3","premium":"30.25","name":"Cy"}]
                """;
        try (Diff d = CsvDiff.compare(json(THREE_ROWS), json(shuffled))) {
            assertEquals(NAMES, d.columns());
            assertTrue(d.summary().identical(), describe(d).toString());
        }
    }

    @Test
    void anEmptyArrayIsATableWithNoRows() {
        try (Diff d = CsvDiff.compare(json("[]"), json("  [ ]  "))) {
            assertEquals(NAMES, d.columns());
            assertEquals(0, d.summary().rows().unchanged());
            assertTrue(d.summary().identical());
        }
    }

    /** The header row count contract of every row source, inherited here. Under the
     *  default of four a names-only header would eat three data rows. */
    @Test
    void aNamesOnlyHeaderKeepsEveryDataRow() {
        Header h = Header.namesOnly(NAMES);
        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(h, new StringReader(THREE_ROWS)),
                DiffSource.ofJsonRows(h, new StringReader(THREE_ROWS)))) {
            assertEquals(NAMES, d.columns());
            assertEquals(3, d.summary().rows().unchanged());
        }
    }

    /* ------------------------------------------- the shared value renderer -- */

    /**
     * The reason there is one renderer and not two. A JSON side and a JDBC side of
     * the same three rows, including a null, a boolean and a decimal, have to be
     * identical, and this is what fails the day someone adds a second rendering rule
     * in the JSON path.
     */
    @Test
    void aJsonSideAndAJdbcSideOfTheSameDataAreIdentical() {
        Header h = Header.builder()
                .column("id", "INTEGER").key().required()
                .column("name", "VARCHAR(20)")
                .column("active", "BOOLEAN")
                .column("premium", "DECIMAL(12,2)")
                .build();

        ResultSet rs = FakeResultSet.of(4, List.of(
                java.util.Arrays.asList(1, "Ann", Boolean.TRUE, new java.math.BigDecimal("10.50")),
                java.util.Arrays.asList(2, null, Boolean.FALSE, new java.math.BigDecimal("20.00"))));

        String sameAsJson = """
                [{"id":"1","name":"Ann","active":true,"premium":"10.50"},
                 {"id":"2","name":null,"active":false,"premium":"20.00"}]
                """;

        try (Diff d = CsvDiff.compare(
                DiffSource.ofResultSet(rs, h),
                DiffSource.ofJsonRows(h, new StringReader(sameAsJson)))) {
            assertTrue(d.summary().identical(), describe(d).toString());
        }
    }

    /**
     * A number keeps the text it was written with. Compared as {@code VARCHAR} on
     * purpose: under {@code DECIMAL} the engine compares by value and 1.50 equals
     * 1.5, which would pass whether or not the literal survived and prove nothing.
     */
    @Test
    void aNumberKeepsItsLiteralTextRatherThanPassingThroughADouble() {
        Header h = Header.builder()
                .column("id", "VARCHAR(4)").key().required()
                .column("amount", "VARCHAR(40)")
                .build();

        String source = """
                [{"id":"a","amount":1.50},
                 {"id":"b","amount":12345678901234567890123},
                 {"id":"c","amount":0.1000},
                 {"id":"d","amount":1e3}]
                """;
        byte[] literal = csv(h, List.of(
                List.of("a", "1.50"),
                List.of("b", "12345678901234567890123"),
                List.of("c", "0.1000"),
                List.of("d", "1e3")));

        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(h, new StringReader(source)), DiffSource.ofBytes(literal))) {
            assertTrue(d.summary().identical(), describe(d).toString());
        }
    }

    /** The values a JSON document can hold that a CSV cell has to carry as text. */
    @Test
    void stringEscapesSurviveIntoTheReport() {
        Header h = Header.namesOnly(List.of("id", "text"));
        String source = """
                [{"id":"1","text":"a \\"quoted\\" word"},
                 {"id":"2","text":"two\\nlines"},
                 {"id":"3","text":"a\\tb"},
                 {"id":"4","text":"\\u4e2d\\u6587 \\ud83d\\ude00"},
                 {"id":"5","text":"back\\\\slash"}]
                """;

        List<List<String>> same = List.of(
                List.of("1", "a \"quoted\" word"),
                List.of("2", "two\nlines"),
                List.of("3", "a\tb"),
                List.of("4", "中文 😀"),
                List.of("5", "back\\slash"));

        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(h, new StringReader(source)),
                DiffSource.ofRows(h, same.iterator()))) {
            assertTrue(d.summary().identical(), describe(d).toString());
        }
    }

    /* -------------------------------------------------------- the refusals -- */

    @Test
    void aMissingKeyIsAnErrorRatherThanAnEmptyCell() {
        CsvDiffException e = assertThrows(CsvDiffException.class, () -> compareToItself("""
                [{"id":"1","name":"Ann","premium":"10.50"},
                 {"id":"2","name":"Bo"}]
                """));
        assertTrue(e.getMessage().contains("row 2"), e.getMessage());
        assertTrue(e.getMessage().contains("premium"), e.getMessage());
    }

    @Test
    void aKeyTheHeaderDoesNotDeclareIsAnError() {
        CsvDiffException e = assertThrows(CsvDiffException.class, () -> compareToItself("""
                [{"id":"1","name":"Ann","premium":"10.50","notes":"extra"}]
                """));
        assertTrue(e.getMessage().contains("notes"), e.getMessage());
        assertTrue(e.getMessage().contains("row 1"), e.getMessage());
    }

    @Test
    void theSameKeyTwiceIsAnError() {
        CsvDiffException e = assertThrows(CsvDiffException.class, () -> compareToItself("""
                [{"id":"1","name":"Ann","name":"Anne","premium":"10.50"}]
                """));
        assertTrue(e.getMessage().contains("twice"), e.getMessage());
    }

    @Test
    void aNestedValueIsRefusedRatherThanRendered() {
        CsvDiffException object = assertThrows(CsvDiffException.class, () -> compareToItself("""
                [{"id":"1","name":{"first":"Ann"},"premium":"10.50"}]
                """));
        assertTrue(object.getMessage().contains("nested object"), object.getMessage());

        CsvDiffException array = assertThrows(CsvDiffException.class, () -> compareToItself("""
                [{"id":"1","name":["Ann"],"premium":"10.50"}]
                """));
        assertTrue(array.getMessage().contains("nested array"), array.getMessage());
    }

    /** An array of arrays is the shape that was deliberately not built, so it has to
     *  fail with a message that says which shape this reads, not with a parse error
     *  about a bracket. */
    @Test
    void anArrayOfArraysSaysWhichShapeThisReads() {
        CsvDiffException e = assertThrows(CsvDiffException.class,
                () -> compareToItself("[[\"1\",\"Ann\",\"10.50\"]]"));
        assertTrue(e.getMessage().contains("array of"), e.getMessage());
        assertTrue(e.getMessage().contains("objects"), e.getMessage());
    }

    /** Truncation must not look like the end of the data. A document cut off by a
     *  failed download would otherwise report every missing row as deleted. */
    @Test
    void aTruncatedDocumentIsAnErrorAndNotAShorterTable() {
        for (String bad : List.of(
                "[{\"id\":\"1\",\"name\":\"Ann\",\"premium\":\"10.50\"}",
                "[{\"id\":\"1\",\"name\":\"Ann\",\"premium\":\"10.5",
                "[{\"id\":\"1\",\"name\":",
                "")) {
            assertThrows(CsvDiffException.class, () -> compareToItself(bad),
                    "should have refused: " + bad);
        }
    }

    @Test
    void trailingContentAndTrailingCommasAreErrors() {
        assertThrows(CsvDiffException.class, () -> compareToItself(THREE_ROWS.trim() + " [1]"));
        assertThrows(CsvDiffException.class, () -> compareToItself("""
                [{"id":"1","name":"Ann","premium":"10.50"},]
                """));
    }

    /** The position is the whole value of the message on a document that is one line
     *  and 15 MB long. */
    @Test
    void anErrorNamesTheLineAndColumn() {
        CsvDiffException e = assertThrows(CsvDiffException.class, () -> compareToItself("""
                [{"id":"1","name":"Ann","premium":"10.50"},
                 {"id":"2","name":"Bo","premium":oops}]
                """));
        assertTrue(e.getMessage().contains("line 2"), e.getMessage());
        assertTrue(e.getMessage().contains("column"), e.getMessage());
    }

    /* ------------------------------------------------------- the plumbing -- */

    /** A value longer than the parser's own buffer, so a token is refilled across a
     *  read boundary rather than assumed to be whole. */
    @Test
    void aValueLongerThanTheReadBufferIsReadWhole() {
        Header h = Header.namesOnly(List.of("id", "text"));
        String big = "x".repeat(20_000);
        String source = "[{\"id\":\"1\",\"text\":\"" + big + "\"}]";

        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(h, new StringReader(source)),
                DiffSource.ofRows(h, List.of(List.of("1", big)).iterator()))) {
            assertTrue(d.summary().identical(), describe(d).toString());
        }
    }

    /** Enough rows to cross the batch flush several times, which is where a source
     *  that held state per batch rather than per row would come apart. */
    @Test
    void manyRowsStreamThroughTheBatchFeed() {
        int n = 20_000;
        StringBuilder json = new StringBuilder("[");
        List<List<String>> rows = new ArrayList<>(n);
        for (int i = 0; i < n; i++) {
            if (i > 0) json.append(',');
            json.append("{\"id\":\"").append(i).append("\",\"name\":\"n").append(i)
                    .append("\",\"premium\":\"").append(i).append(".00\"}");
            rows.add(List.of(String.valueOf(i), "n" + i, i + ".00"));
        }
        json.append(']');

        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(keyed(), new StringReader(json.toString())),
                DiffSource.ofRows(keyed(), rows.iterator()))) {
            assertEquals(n, d.summary().rows().unchanged());
            assertTrue(d.summary().identical());
        }
    }

    @Test
    void aJsonSourceReadsFromAStreamAsUtf8() {
        byte[] utf8 = """
                [{"id":"1","name":"Björn","premium":"10.50"}]
                """.getBytes(StandardCharsets.UTF_8);
        List<List<String>> same = List.of(List.of("1", "Björn", "10.50"));

        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(keyed(), new ByteArrayInputStream(utf8)),
                DiffSource.ofRows(keyed(), same.iterator()))) {
            assertTrue(d.summary().identical(), describe(d).toString());
        }
    }

    @Test
    void aJsonClobIsReadAndItsStreamIsClosed() {
        FakeClob clob = new FakeClob(THREE_ROWS);
        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(keyed(), clob.proxy()), json(THREE_ROWS))) {
            assertTrue(d.summary().identical(), describe(d).toString());
        }
        assertTrue(clob.opened.get(0).closed, "the character stream was left open");
        assertFalse(clob.freed, "the caller's CLOB was freed");
    }

    @Test
    void aReaderIsNotClosedByTheSource() {
        CloseWatcher r = new CloseWatcher(THREE_ROWS);
        try (Diff d = CsvDiff.compare(
                DiffSource.ofJsonRows(keyed(), r), json(THREE_ROWS))) {
            d.summary();
        }
        assertFalse(r.closed, "the caller's reader was closed");
    }

    @Test
    void theSourceDeclaresItsHeaderAndRejectsANullOne() {
        assertEquals(4, DiffSource.ofJsonRows(keyed(), new StringReader("[]"))
                .header().orElseThrow().rowCount());
        assertThrows(CsvDiffException.class,
                () -> DiffSource.ofJsonRows(null, new StringReader("[]")));
        assertThrows(CsvDiffException.class,
                () -> DiffSource.ofJsonRows(keyed(), (Reader) null));
        assertThrows(CsvDiffException.class,
                () -> DiffSource.ofJsonRows(keyed(), (Clob) null));
    }

    /* ---------------------------------------------------------------- utils -- */

    private static void compareToItself(String text) {
        try (Diff d = CsvDiff.compare(json(text), json(THREE_ROWS))) {
            d.summary();
        }
    }

    private static List<String> describe(Diff d) {
        return d.rows()
                .map(r -> r.kind() + " " + r.sourceRow() + "->" + r.targetRow() + " "
                        + r.cells().stream()
                                .map(c -> c.source().orElse("<none>") + "|" + c.target().orElse("<none>"))
                                .toList())
                .toList();
    }

    /**
     * The file half of a comparison, built by hand.
     *
     * <p>It quotes because it has to: a declared type of {@code DECIMAL(12,2)}
     * carries the delimiter, and joining a header row with commas turns three
     * columns into four. That is the mistake {@code DiffSource.withHeader} exists to
     * stop a caller making, and it was made here first.
     */
    private static byte[] csv(Header header, List<List<String>> rows) {
        StringBuilder sb = new StringBuilder();
        for (List<String> row : header.toRows()) line(sb, row);
        for (List<String> row : rows) line(sb, row);
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }

    private static void line(StringBuilder sb, List<String> row) {
        for (int i = 0; i < row.size(); i++) {
            if (i > 0) sb.append(',');
            String cell = row.get(i);
            if (cell.indexOf(',') >= 0) {
                sb.append('"').append(cell.replace("\"", "\"\"")).append('"');
            } else {
                sb.append(cell);
            }
        }
        sb.append('\n');
    }

    private static final class CloseWatcher extends StringReader {
        boolean closed;

        CloseWatcher(String s) {
            super(s);
        }

        @Override
        public void close() {
            closed = true;
            super.close();
        }
    }

    /** A {@link Clob} over a string, as a proxy, for the reason {@link FakeResultSet}
     *  is one. */
    private static final class FakeClob implements InvocationHandler {
        private final String text;
        final List<CloseWatcher> opened = new ArrayList<>();
        boolean freed;

        FakeClob(String text) {
            this.text = text;
        }

        Clob proxy() {
            return (Clob) Proxy.newProxyInstance(JsonRowsTest.class.getClassLoader(),
                    new Class<?>[] {Clob.class}, this);
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            switch (method.getName()) {
                case "length":
                    return (long) text.length();
                case "getCharacterStream": {
                    CloseWatcher w = new CloseWatcher(text);
                    opened.add(w);
                    return w;
                }
                case "free":
                    freed = true;
                    return null;
                case "toString":
                    return "FakeClob(" + text.length() + " chars)";
                case "hashCode":
                    return System.identityHashCode(proxy);
                case "equals":
                    return proxy == args[0];
                default:
                    throw new UnsupportedOperationException(method.getName());
            }
        }
    }
}
