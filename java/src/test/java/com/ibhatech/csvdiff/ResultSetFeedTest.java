package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.sql.ResultSet;
import java.sql.Timestamp;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The row feed: a result set and a file reach the same parser.
 *
 * <p>The load bearing assertion is {@link #aResultSetDiffsTheSameAsTheEquivalentCsv()}.
 * The feed encodes rows as CSV into the staging buffer, so the claim that it adds
 * no semantics of its own is checkable: the same data through the two paths must
 * produce the same report, cell for cell, and it does.
 */
class ResultSetFeedTest {

    private static final Header HEADER = Header.builder()
            .column("account_id", "VARCHAR(20)").key().required()
            .column("period", "CHAR(7)").key().required()
            .column("customer_name", "VARCHAR(60)").required()
            .column("premium_amount", "DECIMAL(12,2)")
            .column("status", "VARCHAR(12)")
            .build();

    private static List<List<Object>> sourceRows() {
        return List.of(
                row("ACC-001", "2026-01", "Alice Anderson", new BigDecimal("8259.65"), "ACTIVE"),
                row("ACC-002", "2026-02", "Smith, John", new BigDecimal("178.55"), "PENDING"),
                row("ACC-003", "2026-03", "O\"Brien Holdings", new BigDecimal("7141.85"), "LAPSED"),
                row("ACC-004", "2026-04", "Multi\nline name", new BigDecimal("42.00"), "ACTIVE"));
    }

    private static List<Object> row(Object... values) {
        return Arrays.asList(values);
    }

    /* ------------------------------------------------------- the main claim -- */

    @Test
    void aResultSetDiffsTheSameAsTheEquivalentCsv() {
        List<List<Object>> source = sourceRows();
        List<List<Object>> target = new ArrayList<>(source);
        // one changed cell, one deleted row, one added row: every row kind appears
        target.set(0, row("ACC-001", "2026-01", "Alice Anderson", new BigDecimal("9000.00"),
                "CANCELLED"));
        target.remove(1);
        target.add(row("ACC-005", "2026-05", "New Customer", new BigDecimal("10.50"), "ACTIVE"));

        List<DiffRow> viaResultSet;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofResultSet(resultSet(source), HEADER),
                DiffSource.ofResultSet(resultSet(target), Header.namesOnly(HEADER.names())))) {
            viaResultSet = d.rows().toList();
        }

        List<DiffRow> viaCsv;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(csvOf(HEADER, source)),
                DiffSource.ofBytes(csvOf(Header.namesOnly(HEADER.names()), target)))) {
            viaCsv = d.rows().toList();
        }

        assertEquals(viaCsv.size(), viaResultSet.size(), "report row count");
        assertTrue(viaCsv.size() >= 5, "expected every row kind to appear");
        for (int i = 0; i < viaCsv.size(); i++) {
            assertEquals(viaCsv.get(i), viaResultSet.get(i), "row " + i);
        }
    }

    /* ------------------------------------------------- what the header does -- */

    @Test
    void theHeaderSuppliesTheKeyColumnsThatJdbcCannot() {
        try (Diff d = CsvDiff.compare(
                DiffSource.ofResultSet(resultSet(sourceRows()), HEADER),
                DiffSource.ofResultSet(resultSet(sourceRows()), Header.namesOnly(HEADER.names())))) {

            assertEquals(List.of(0, 1), d.keyColumns());
            assertEquals(HEADER.names(), d.columns());
            assertTrue(d.summary().identical());
            // Keyed matching, not the all-keys similarity path of spec 6.4.
            assertFalse(d.summary().matching().allKeys());
        }
    }

    @Test
    void aHeaderNarrowerThanTheResultSetIsRefusedBeforeAnythingIsParsed() {
        Header tooNarrow = Header.builder().column("account_id").column("period").build();
        CsvDiffException e = assertThrows(CsvDiffException.class, () -> {
            try (Diff d = CsvDiff.compare(
                    DiffSource.ofResultSet(resultSet(sourceRows()), tooNarrow),
                    DiffSource.ofResultSet(resultSet(sourceRows()), tooNarrow))) {
                d.summary();
            }
        });
        assertTrue(e.getMessage().contains("the header declares 2 columns"), e.getMessage());
    }

    @Test
    void theRequiredMarkersInTheHeaderProduceFindings() {
        List<List<Object>> rows = List.of(
                row("ACC-001", "2026-01", null, new BigDecimal("1.00"), "ACTIVE"));
        try (Diff d = CsvDiff.compare(
                DiffSource.ofResultSet(resultSet(rows), HEADER),
                DiffSource.ofResultSet(resultSet(rows), Header.namesOnly(HEADER.names())))) {

            List<DiffRow> report = d.rows().toList();
            assertEquals(1, report.size());
            List<Finding> findings = report.get(0).findings();
            assertEquals(1, findings.size());
            assertEquals(FindingKind.REQUIRED_EMPTY, findings.get(0).kind());
            assertEquals("customer_name", findings.get(0).name());
            // A finding never aborts and is never dropped by changesOnly.
            assertEquals(1, d.rows(RowOptions.changes()).count());
        }
    }

    /* ----------------------------------------------------- value rendering -- */

    @Test
    void valuesAreRenderedByTheDocumentedRulesNotByTheDriver() {
        Header h = Header.builder()
                .column("id", "VARCHAR(10)").key()
                .column("amount", "DECIMAL(12,2)")
                .column("when", "TIMESTAMP")
                .column("flag", "BOOLEAN")
                .column("note", "VARCHAR(40)")
                .build();

        List<List<Object>> rows = List.of(row(
                "A1",
                new BigDecimal("1.50"),
                Timestamp.valueOf("2026-01-31 14:22:05"),
                Boolean.TRUE,
                null));

        try (Diff d = CsvDiff.compare(
                DiffSource.ofResultSet(resultSet(rows), h),
                DiffSource.ofResultSet(resultSet(rows), Header.namesOnly(h.names())))) {

            List<DiffCell> cells = d.rows().toList().get(0).cells();
            assertEquals("1.50", cells.get(1).target().orElseThrow(),
                    "BigDecimal keeps its scale: 1.50 must not become 1.5");
            assertEquals("2026-01-31T14:22:05", cells.get(2).target().orElseThrow());
            assertEquals("TRUE", cells.get(3).target().orElseThrow());
            assertEquals("", cells.get(4).target().orElseThrow(), "SQL NULL is an empty field");
        }
    }

    @Test
    void binaryIsRefusedRatherThanQuietlyEncoded() {
        Header h = Header.builder().column("id").key().column("blob").build();
        List<List<Object>> rows = List.of(row("A1", new byte[] {1, 2, 3}));

        CsvDiffException e = assertThrows(CsvDiffException.class, () -> {
            try (Diff d = CsvDiff.compare(
                    DiffSource.ofResultSet(resultSet(rows), h),
                    DiffSource.ofResultSet(resultSet(rows), h))) {
                d.summary();
            }
        });
        assertTrue(e.getMessage().contains("binary"), e.getMessage());
    }

    /* ------------------------------------------------------------ batching -- */

    /** The batch size is a boundary, not a semantic: the same rows fed one per call
     *  and a thousand per call must produce the same report. A flush lands wherever
     *  it lands, including inside a quoted multiline value, which is what the
     *  parser's resumability is for. */
    @Test
    void theBatchSizeDoesNotChangeTheReport() {
        List<List<Object>> source = sourceRows();
        List<DiffRow> reference = null;

        for (int batch : new int[] {1, 2, 1000}) {
            RowFeedOptions feed = RowFeedOptions.defaults().withBatchRows(batch);
            DiffOptions options = DiffOptions.builder().stagingBytes(4096).build();
            try (Diff d = CsvDiff.compare(
                    DiffSource.ofResultSet(resultSet(source), HEADER, feed),
                    DiffSource.ofResultSet(resultSet(source), Header.namesOnly(HEADER.names()), feed),
                    options)) {

                List<DiffRow> rows = d.rows().toList();
                if (reference == null) reference = rows;
                else assertEquals(reference, rows, "batch size " + batch + " changed the report");
            }
        }
        assertEquals(4, reference.size());
    }

    /* -------------------------------------------------- the non JDBC feed -- */

    @Test
    void anyRowIteratorFeedsTheSameWay() {
        List<List<String>> rows = List.of(
                List.of("ACC-001", "2026-01", "Alice", "1.00", "ACTIVE"),
                List.of("ACC-002", "2026-02", "Bob", "2.00", "ACTIVE"));

        try (Diff d = CsvDiff.compare(
                DiffSource.ofRows(HEADER, rows.iterator()),
                DiffSource.ofRows(Header.namesOnly(HEADER.names()), rows.iterator()))) {
            assertTrue(d.summary().identical());
            assertEquals(2, d.rows().count());
        }
    }

    /* ---------------------------------------------------------------- utils -- */

    /** The fake reports the width its rows actually have, which is what makes the
     *  header width check a real check rather than one that always agrees. */
    private static ResultSet resultSet(List<List<Object>> rows) {
        return FakeResultSet.of(rows.get(0).size(), rows);
    }

    private static byte[] csvOf(Header header, List<List<Object>> rows) {
        StringBuilder sb = new StringBuilder();
        for (List<String> headerRow : header.toRows()) appendRow(sb, headerRow);
        for (List<Object> row : rows) {
            List<String> values = new ArrayList<>(row.size());
            for (int i = 0; i < row.size(); i++) {
                try {
                    values.add(SqlValues.render(row.get(i), i + 1));
                } catch (java.sql.SQLException e) {
                    throw new IllegalStateException(e);
                }
            }
            appendRow(sb, values);
        }
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }

    private static void appendRow(StringBuilder sb, List<String> values) {
        for (int i = 0; i < values.size(); i++) {
            if (i > 0) sb.append(',');
            String v = values.get(i);
            if (v.indexOf(',') >= 0 || v.indexOf('"') >= 0 || v.indexOf('\n') >= 0
                    || v.indexOf('\r') >= 0) {
                sb.append('"').append(v.replace("\"", "\"\"")).append('"');
            } else {
                sb.append(v);
            }
        }
        sb.append('\n');
    }
}
