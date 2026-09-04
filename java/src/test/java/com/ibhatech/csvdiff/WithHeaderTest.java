package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * {@code DiffSource.withHeader}: header rows written ahead of data that carries
 * none.
 *
 * <p>The shape it serves, from {@code specs/input-options.md} use cases 2 and 3: the
 * original side lives in a CLOB holding data rows only, or a name row only, while
 * which columns are keys and what their types are lives in a metadata table. The
 * caller states the header once and the engine sees one ordinary CSV.
 *
 * <p>What has to hold is that the wrapped source is indistinguishable from a file
 * with the header already in it, <em>and</em> that the header row count the parse
 * uses is the one that was written. The second is the contract from
 * {@link DiffOptions#headerLayout(Header)}, and it is inherited here rather than
 * reopened: a names-only header wrapped under the default of four rows is exactly
 * the defect that contract was built for.
 */
class WithHeaderTest {

    private static final List<String> NAMES = List.of("id", "name");

    private static Header keyed() {
        return Header.builder()
                .column("id", "INTEGER").key().required()
                .column("name", "VARCHAR(20)")
                .build();
    }

    private static List<List<String>> fiveRows() {
        return List.of(
                List.of("1", "a"),
                List.of("2", "b"),
                List.of("3", "c"),
                List.of("4", "d"),
                List.of("5", "e"));
    }

    /* ------------------------------------------------ the same as a whole file -- */

    /**
     * The load bearing equivalence. A wrapped body and a file with the header
     * already in it are the same bytes to the same parser, so they must produce the
     * same report down to the cell.
     */
    @Test
    void aWrappedBodyReportsWhatTheWholeCsvDoes() {
        Header h = keyed();
        List<List<String>> source = fiveRows();
        List<List<String>> target = changed(source);

        List<DiffRow> viaWrapped;
        try (Diff d = CsvDiff.compare(
                DiffSource.withHeader(h, DiffSource.ofString(body(source))),
                DiffSource.withHeader(h, DiffSource.ofString(body(target))))) {
            viaWrapped = d.rows().toList();
        }

        List<DiffRow> viaCsv;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(csv(h, source)),
                DiffSource.ofBytes(csv(h, target)))) {
            viaCsv = d.rows().toList();
        }

        assertEquals(viaCsv.size(), viaWrapped.size(), "report row count");
        for (int i = 0; i < viaCsv.size(); i++) {
            assertEquals(viaCsv.get(i).kind(), viaWrapped.get(i).kind(), "row " + i + " change kind");
            assertEquals(values(viaCsv.get(i)), values(viaWrapped.get(i)), "row " + i + " cells");
        }
    }

    /** The header is data to the engine, so the KEY row it writes is the KEY row the
     *  engine reads. Without that the comparison would quietly take the all-keys
     *  similarity path of spec 6.4 instead of matching on the key. */
    @Test
    void theKeyMarkersInTheWrittenHeaderAreTheOnesTheEngineReads() {
        Header h = keyed();
        try (Diff d = CsvDiff.compare(
                DiffSource.withHeader(h, DiffSource.ofString(body(fiveRows()))),
                DiffSource.withHeader(h, DiffSource.ofString(body(fiveRows()))))) {
            assertEquals(NAMES, d.columns());
            assertEquals(List.of(0), d.keyColumns(), "the KEY marker row was written and read");
            assertEquals(5, d.summary().rows().unchanged());
        }
    }

    /** The whole point of {@code header()}: one row written, one row parsed. Under
     *  the default of four this would eat the name row and three data rows and say
     *  nothing, which is the defect in handoff section 3.1. */
    @Test
    void aNamesOnlyHeaderKeepsEveryDataRow() {
        Header h = Header.namesOnly(NAMES);
        try (Diff d = CsvDiff.compare(
                DiffSource.withHeader(h, DiffSource.ofString(body(fiveRows()))),
                DiffSource.withHeader(h, DiffSource.ofString(body(fiveRows()))))) {
            assertEquals(NAMES, d.columns(), "the name row is the header, not a data row");
            assertEquals(5, d.summary().rows().unchanged(), "no data row absorbed into the header");
        }
    }

    /** A header on one side and a file carrying its own on the other, which is use
     *  case 2 exactly: a CLOB original against an uploaded CSV. */
    @Test
    void aWrappedSourceComparesAgainstAFileThatCarriesItsOwnHeader() {
        Header h = keyed();
        try (Diff d = CsvDiff.compare(
                DiffSource.withHeader(h, DiffSource.ofString(body(fiveRows()))),
                DiffSource.ofBytes(csv(h, changed(fiveRows()))))) {
            List<DiffRow> rows = d.rows().toList();
            assertEquals(NAMES, d.columns());
            assertTrue(rows.stream().anyMatch(r -> r.kind() == ChangeKind.MODIFIED),
                    "the changed row: " + rows);
        }
    }

    /* --------------------------------------------------------- the declaration -- */

    @Test
    void theWrapperDeclaresTheHeaderItWrites() {
        Header h = Header.namesOnly(NAMES);
        DiffSource s = DiffSource.withHeader(h, DiffSource.ofBytes(new byte[0]));
        assertEquals(1, s.header().orElseThrow().rowCount());
        assertEquals(NAMES, s.header().orElseThrow().names());
    }

    /** Inherited, not reimplemented: a caller who wrote the count down twice had one
     *  of the two in mind, and guessing which is how the original defect returns. */
    @Test
    void statingADifferentHeaderRowCountIsRejected() {
        Header h = Header.namesOnly(NAMES);
        CsvDiffException e = assertThrows(CsvDiffException.class, () ->
                CsvDiff.compare(
                        DiffSource.withHeader(h, DiffSource.ofString(body(fiveRows()))),
                        DiffSource.withHeader(h, DiffSource.ofString(body(fiveRows()))),
                        DiffOptions.builder().headerRows(4).build()));
        assertTrue(e.getMessage().contains("1 header row"), e.getMessage());
    }

    /** Two headers would be written and one would be parsed, and the second would
     *  arrive as data rows: three phantom rows in the report, or a ragged row error
     *  if the widths disagreed. Refused where the mistake is made. */
    @Test
    void wrappingASourceThatAlreadyDeclaresAHeaderIsRejected() {
        Header h = keyed();
        CsvDiffException e = assertThrows(CsvDiffException.class, () ->
                DiffSource.withHeader(h,
                        DiffSource.ofRows(Header.namesOnly(NAMES), fiveRows().iterator())));
        assertTrue(e.getMessage().contains("already declares"), e.getMessage());
    }

    @Test
    void aNullHeaderOrBodyIsRejectedWhereItIsGiven() {
        assertThrows(CsvDiffException.class,
                () -> DiffSource.withHeader(null, DiffSource.ofBytes(new byte[0])));
        assertThrows(CsvDiffException.class, () -> DiffSource.withHeader(keyed(), null));
    }

    /* -------------------------------------------------------------- the writing -- */

    /** The header rows go through {@link CsvWriter}, so a column name containing the
     *  delimiter is quoted and the row does not gain a column. Building the header
     *  by string concatenation is what this exists to stop, and this is why. */
    @Test
    void aColumnNameContainingTheDelimiterIsQuoted() {
        Header h = Header.namesOnly(List.of("last, first", "amount"));
        String data = "Ann Lee,10\nBo Ng,20\n";
        try (Diff d = CsvDiff.compare(
                DiffSource.withHeader(h, DiffSource.ofString(data)),
                DiffSource.withHeader(h, DiffSource.ofString(data)))) {
            assertEquals(List.of("last, first", "amount"), d.columns());
            assertEquals(2, d.summary().rows().unchanged());
        }
    }

    /** The dialect the header is written in has to be the dialect the engine parses,
     *  and for anything but the default that means saying it in both places. */
    @Test
    void theHeaderIsWrittenInTheDialectTheCallerChose() {
        Header h = keyed();
        RowFeedOptions semicolon = new RowFeedOptions(1000, (byte) ';', (byte) '"');
        String data = "1;a\n2;b\n";
        try (Diff d = CsvDiff.compare(
                DiffSource.withHeader(h, DiffSource.ofString(data), semicolon),
                DiffSource.withHeader(h, DiffSource.ofString(data), semicolon),
                DiffOptions.builder().delimiter(';').build())) {
            assertEquals(NAMES, d.columns());
            assertEquals(List.of(0), d.keyColumns());
            assertEquals(2, d.summary().rows().unchanged());
        }
    }

    /* ------------------------------------------------------------- the size hint -- */

    @Test
    void theHintCoversTheHeaderAndTheBody() {
        byte[] data = body(fiveRows()).getBytes(StandardCharsets.UTF_8);
        long hint = DiffSource.withHeader(keyed(), DiffSource.ofBytes(data)).sizeHint();
        assertTrue(hint > data.length, "the header rows are bytes too: " + hint);
        assertTrue(hint < data.length + 200, "and there are only four of them: " + hint);
    }

    /** A hint covering the header alone would be worse than none: it would size the
     *  reservation to a few hundred bytes and grow from there. */
    @Test
    void anUnknownBodyLeavesTheWholeThingUnknown() {
        assertEquals(0, DiffSource.withHeader(keyed(),
                DiffSource.ofStream(new ByteArrayInputStream(new byte[0]))).sizeHint());
    }

    /* ---------------------------------------------------------------- utils -- */

    private static List<List<String>> changed(List<List<String>> rows) {
        List<List<String>> out = new ArrayList<>(rows);
        out.set(1, List.of("2", "CHANGED"));
        out.remove(3);
        out.add(List.of("9", "z"));
        return out;
    }

    private static List<String> values(DiffRow row) {
        List<String> out = new ArrayList<>();
        for (DiffCell c : row.cells()) {
            out.add(c.source().orElse("<none>") + "|" + c.target().orElse("<none>"));
        }
        return out;
    }

    /** The data rows alone, as a CLOB would hold them. */
    private static String body(List<List<String>> rows) {
        StringBuilder sb = new StringBuilder();
        for (List<String> row : rows) sb.append(String.join(",", row)).append('\n');
        return sb.toString();
    }

    /** The same rows with the header already in front, as a file holds them. */
    private static byte[] csv(Header header, List<List<String>> rows) {
        StringBuilder sb = new StringBuilder();
        for (List<String> row : header.toRows()) sb.append(String.join(",", row)).append('\n');
        return (sb + body(rows)).getBytes(StandardCharsets.UTF_8);
    }
}
