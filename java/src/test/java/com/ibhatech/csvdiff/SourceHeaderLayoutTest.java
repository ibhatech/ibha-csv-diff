package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The source side's header row count comes from the source, not from the default.
 *
 * <p>The defect these exist for: {@code JniDiff} took the source side's header row
 * count from {@link DiffOptions} alone, so a {@link Header#namesOnly} row source
 * parsed under the default of four consumed its name row and the first three data
 * rows as a header. The fourth data row became the column names, three rows vanished,
 * and the summary said {@code identical=true}. Every test before this one used
 * {@code namesOnly} on the target side, where the count is auto-detected, which is
 * why the gate stayed green.
 */
class SourceHeaderLayoutTest {

    private static final List<String> NAMES = List.of("id", "name");

    private static List<List<String>> fiveRows() {
        return List.of(
                List.of("1", "a"),
                List.of("2", "b"),
                List.of("3", "c"),
                List.of("4", "d"),
                List.of("5", "e"));
    }

    /* ------------------------------------------------------- the regression -- */

    @Test
    void aNamesOnlyRowSourceOnTheSourceSideKeepsEveryDataRow() {
        Header h = Header.namesOnly(NAMES);
        try (Diff d = CsvDiff.compare(
                DiffSource.ofRows(h, fiveRows().iterator()),
                DiffSource.ofRows(h, fiveRows().iterator()))) {
            assertEquals(NAMES, d.columns(), "the name row is the header, not a data row");
            assertEquals(5, d.summary().rows().unchanged(), "no data row absorbed into the header");
            assertEquals(0, d.summary().rows().deleted());
            assertEquals(0, d.summary().rows().added());
            // A names-only header declares no key column, so this takes the all-keys
            // similarity path of spec 6.4. Worth asserting: it is the visible
            // consequence of the header being read as one row rather than four.
            assertTrue(d.summary().matching().allKeys(), "a names-only header declares no key");
        }
    }

    @Test
    void aNamesOnlyRowSourceMatchesTheEquivalentCsv() {
        List<List<String>> source = fiveRows();
        List<List<String>> target = new ArrayList<>(source);
        target.set(1, List.of("2", "CHANGED"));
        target.remove(3);
        target.add(List.of("9", "z"));

        Header h = Header.namesOnly(NAMES);

        List<DiffRow> viaRows;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofRows(h, source.iterator()),
                DiffSource.ofRows(h, target.iterator()))) {
            viaRows = d.rows().toList();
        }

        List<DiffRow> viaCsv;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(csv(NAMES, source)),
                DiffSource.ofBytes(csv(NAMES, target)),
                DiffOptions.builder().headerRows(1).build())) {
            viaCsv = d.rows().toList();
        }

        assertEquals(viaCsv.size(), viaRows.size(), "report row count");
        for (int i = 0; i < viaCsv.size(); i++) {
            assertEquals(viaCsv.get(i).kind(), viaRows.get(i).kind(), "row " + i + " change kind");
            assertEquals(values(viaCsv.get(i)), values(viaRows.get(i)), "row " + i + " cells");
        }
    }

    /* ------------------------------------------------- the unchanged cases -- */

    @Test
    void aFourRowHeaderRowSourceStillParsesAsFour() {
        Header h = Header.builder()
                .column("id", "INTEGER").key().required()
                .column("name", "VARCHAR(20)")
                .build();
        try (Diff d = CsvDiff.compare(
                DiffSource.ofRows(h, fiveRows().iterator()),
                DiffSource.ofRows(h, fiveRows().iterator()))) {
            assertEquals(NAMES, d.columns());
            assertEquals(5, d.summary().rows().unchanged());
            assertEquals(List.of(0), d.keyColumns(), "the KEY marker row was read");
        }
    }

    @Test
    void aByteSourceStillTakesItsHeaderRowCountFromTheOptions() {
        byte[] bytes = csv(NAMES, fiveRows());
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(bytes), DiffSource.ofBytes(bytes),
                DiffOptions.builder().headerRows(1).build())) {
            assertEquals(NAMES, d.columns());
            assertEquals(5, d.summary().rows().unchanged());
        }
    }

    /* ------------------------------------------------------ the disagreement -- */

    @Test
    void statingADifferentHeaderRowCountIsRejectedRatherThanResolved() {
        Header h = Header.namesOnly(NAMES);
        CsvDiffException e = assertThrows(CsvDiffException.class, () ->
                CsvDiff.compare(
                        DiffSource.ofRows(h, fiveRows().iterator()),
                        DiffSource.ofRows(h, fiveRows().iterator()),
                        DiffOptions.builder().headerRows(4).build()));
        assertTrue(e.getMessage().contains("1 header row"), e.getMessage());
        assertTrue(e.getMessage().contains("headerRows(4)"), e.getMessage());
    }

    @Test
    void statingTheSameHeaderRowCountIsFine() {
        Header h = Header.namesOnly(NAMES);
        try (Diff d = CsvDiff.compare(
                DiffSource.ofRows(h, fiveRows().iterator()),
                DiffSource.ofRows(h, fiveRows().iterator()),
                DiffOptions.builder().headerRows(1).build())) {
            assertEquals(5, d.summary().rows().unchanged());
        }
    }

    /* ------------------------------------------------------- the layout rule -- */

    @Test
    void theLayoutIsDerivedFromTheRowCount() {
        assertEquals(new HeaderLayout(4, 1, 2, 3, 4), DiffOptions.defaults().headerLayout());
        assertEquals(new HeaderLayout(1, 1, 0, 0, 1),
                DiffOptions.defaults().headerLayout(Header.namesOnly(NAMES)));
        assertEquals(new HeaderLayout(0, 0, 0, 0, 0),
                DiffOptions.builder().headerRows(0).build().headerLayout());
    }

    @Test
    void aStatedRowPositionSurvivesTheReconciliation() {
        HeaderLayout l = DiffOptions.builder().nameRow(1).keyRow(0).build()
                .headerLayout(Header.namesOnly(NAMES));
        assertEquals(1, l.rows());
        assertEquals(1, l.nameRow());
        assertEquals(0, l.keyRow(), "a stated position is taken as given, not re-derived");
    }

    @Test
    void aByteSourceDeclaresNoHeaderOfItsOwn() {
        assertTrue(DiffSource.ofBytes(new byte[0]).header().isEmpty());
        assertEquals(Header.namesOnly(NAMES).rowCount(),
                DiffSource.ofRows(Header.namesOnly(NAMES), List.<List<String>>of().iterator())
                        .header().orElseThrow().rowCount());
    }

    @Test
    void headerRowsIsOnlyStatedWhenTheCallerStatesIt() {
        assertEquals(false, DiffOptions.defaults().headerRowsStated());
        assertEquals(true, DiffOptions.builder().headerRows(4).build().headerRowsStated());
    }

    /* ---------------------------------------------------------------- utils -- */

    private static List<String> values(DiffRow row) {
        List<String> out = new ArrayList<>();
        for (DiffCell c : row.cells()) {
            out.add(c.source().orElse("<none>") + "|" + c.target().orElse("<none>"));
        }
        return out;
    }

    private static byte[] csv(List<String> names, List<List<String>> rows) {
        StringBuilder sb = new StringBuilder();
        sb.append(String.join(",", names)).append('\n');
        for (List<String> row : rows) sb.append(String.join(",", row)).append('\n');
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }
}
