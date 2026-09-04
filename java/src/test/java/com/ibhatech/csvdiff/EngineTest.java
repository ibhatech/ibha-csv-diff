package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/** The binding against the engine's own contract: what it reports, what it emits,
 *  and how it fails. */
class EngineTest {

    private static byte[] utf8(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    private static Diff tiny() {
        return CsvDiff.compare(Fixtures.path("tiny_source.csv"), Fixtures.path("tiny_target.csv"));
    }

    @Test
    void reportsTheEngineVersionItWasBuiltAgainst() {
        assertTrue(CsvDiff.engineVersion().matches("\\d+\\.\\d+\\.\\d+"), CsvDiff.engineVersion());
    }

    @Test
    void readsColumnsAndKeysFromTheSourceSchema() {
        try (Diff d = tiny()) {
            assertEquals(12, d.columns().size());
            assertEquals("account_id", d.columns().get(0));
            assertEquals("effective_date", d.columns().get(11));
            assertEquals(List.of(0, 1), d.keyColumns());
        }
    }

    /**
     * The summary comes from the summary emitter, not from the stats struct, and
     * this is what makes the difference visible: the cell counters accumulate as a
     * cursor advances, so a summary read after two full walks would report double
     * on a struct read and reports the truth here.
     */
    @Test
    void theSummaryIsOfExactlyOnePassHoweverOftenTheReportWasWalked() {
        try (Diff d = tiny()) {
            DiffSummary before = d.summary();
            d.rows().count();
            d.rows().count();

            DiffSummary after = DiffSummary.parse(d.emit(EmitFormat.SUMMARY));
            assertEquals(before.cells().changed(), after.cells().changed());
            assertTrue(before.cells().changed() > 0);
        }
    }

    @Test
    void theSummaryAgreesWithTheRowsItSummarizes() {
        try (Diff d = tiny()) {
            DiffSummary s = d.summary();
            List<DiffRow> rows = d.rows().toList();

            assertEquals(s.rows().report(), rows.size());
            assertEquals(s.rows().unchanged(),
                    rows.stream().filter(r -> r.kind() == ChangeKind.UNCHANGED).count());
            assertEquals(s.rows().modified(),
                    rows.stream().filter(r -> r.kind() == ChangeKind.MODIFIED).count());
            assertEquals(s.rows().added(),
                    rows.stream().filter(r -> r.kind() == ChangeKind.ADDED).count());
            assertEquals(s.rows().deleted(),
                    rows.stream().filter(r -> r.kind() == ChangeKind.DELETED).count());
            assertEquals(s.rows().moved(), rows.stream().filter(DiffRow::moved).count());
            assertEquals(s.cells().changed(),
                    rows.stream().mapToLong(DiffRow::changedCells).sum());
            assertEquals(s.columns().compared(), d.columns().size());
            assertFalse(s.identical());
        }
    }

    @Test
    void everyEmitterProducesItsFormat() {
        try (Diff d = tiny()) {
            String jsonl = new String(d.emit(EmitFormat.JSONL, EmitOptions.changes()),
                    StandardCharsets.UTF_8);
            assertTrue(jsonl.startsWith("{\"schemaVersion\":"), jsonl.substring(0, 40));
            assertTrue(jsonl.endsWith("\n"));

            String csv = new String(d.emit(EmitFormat.CSV, EmitOptions.changes()),
                    StandardCharsets.UTF_8);
            assertTrue(csv.lines().findFirst().orElse("").contains("account_id"), csv);

            String html = new String(d.emit(EmitFormat.HTML, EmitOptions.changes()),
                    StandardCharsets.UTF_8);
            assertTrue(html.contains("<table"), "expected a table");
            assertTrue(html.contains("ibha-csvd-"), "expected the default class prefix");

            String summary = new String(d.emit(EmitFormat.SUMMARY), StandardCharsets.UTF_8);
            assertTrue(summary.startsWith("{\"schemaVersion\":"));
        }
    }

    /** The two pass sizing must produce exactly the bytes the first pass measured,
     *  and emitTo must produce exactly what emit does. */
    @Test
    void emitToWritesTheSameBytesAsEmit() {
        try (Diff d = tiny()) {
            byte[] direct = d.emit(EmitFormat.JSONL, EmitOptions.changes());
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            long n = d.emitTo(out, EmitFormat.JSONL, EmitOptions.changes());

            assertEquals(direct.length, n);
            assertArrayEquals(direct, out.toByteArray());
        }
    }

    @Test
    void maxRowsBoundsTheHtmlReport() {
        try (Diff d = tiny()) {
            byte[] all = d.emit(EmitFormat.HTML, EmitOptions.changes());
            byte[] capped = d.emit(EmitFormat.HTML, EmitOptions.changes().withMaxRows(2));
            assertTrue(capped.length < all.length);
        }
    }

    /**
     * A mistyped class prefix costs the caller the call, not the diff. The engine
     * holds one error and the first one wins, so an engine level refusal would
     * abort a comparison that was otherwise complete and every later call on it.
     */
    @Test
    void aBadClassPrefixIsRefusedWithoutTouchingTheEngine() {
        try (Diff d = tiny()) {
            assertThrows(CsvDiffException.class,
                    () -> EmitOptions.defaults().withClassPrefix("9nope"));
            assertThrows(CsvDiffException.class,
                    () -> EmitOptions.defaults().withClassPrefix("has space"));

            // and the handle still works
            assertTrue(d.emit(EmitFormat.SUMMARY).length > 0);
            String html = new String(
                    d.emit(EmitFormat.HTML, EmitOptions.changes().withClassPrefix("acme-")),
                    StandardCharsets.UTF_8);
            assertTrue(html.contains("acme-table"), html.substring(0, Math.min(200, html.length())));
        }
    }

    /* ---------------------------------------------------------------- errors -- */

    private static final String HEADER_4 = """
            KEY,,
            REQUIRED,REQUIRED,
            VARCHAR(10),VARCHAR(10),INTEGER
            id,name,qty
            """;

    @Test
    void aDuplicateKeyIsAnErrorThatNamesTheKeyAndBothRows() {
        byte[] source = utf8(HEADER_4 + "A,alice,1\nA,anne,2\n");
        byte[] target = utf8(HEADER_4 + "A,alice,1\nB,bob,2\n");

        CsvDiffException e = assertThrows(CsvDiffException.class,
                () -> CsvDiff.compare(DiffSource.ofBytes(source), DiffSource.ofBytes(target)));
        assertEquals("DUPLICATE_KEY", e.status());
        assertTrue(e.getMessage().contains("A"), e.getMessage());
    }

    @Test
    void reorderedColumnsAreAHardError() {
        byte[] source = utf8(HEADER_4 + "A,alice,1\n");
        byte[] target = utf8("name,id,qty\nalice,A,1\n");

        CsvDiffException e = assertThrows(CsvDiffException.class,
                () -> CsvDiff.compare(DiffSource.ofBytes(source), DiffSource.ofBytes(target)));
        assertEquals("COLUMN_ORDER", e.status(), e.getMessage());
    }

    /** Findings are output, not errors: an empty REQUIRED cell must not abort. */
    @Test
    void aValidationFindingIsOutputRatherThanAnError() {
        byte[] source = utf8(HEADER_4 + "A,alice,1\n");
        byte[] target = utf8(HEADER_4 + "A,,1\n");

        try (Diff d = CsvDiff.compare(DiffSource.ofBytes(source), DiffSource.ofBytes(target))) {
            List<DiffRow> rows = d.rows().toList();
            assertEquals(1, rows.size());
            assertEquals(FindingKind.REQUIRED_EMPTY, rows.get(0).findings().get(0).kind());
            assertEquals(1, d.summary().findings().requiredEmpty());
            assertTrue(d.summary().findings().enabled());
        }
    }

    @Test
    void tooLongCarriesTheDeclaredLimit() {
        byte[] source = utf8(HEADER_4 + "A,alice,1\n");
        byte[] target = utf8(HEADER_4 + "A,a-name-far-longer-than-ten,1\n");

        try (Diff d = CsvDiff.compare(DiffSource.ofBytes(source), DiffSource.ofBytes(target))) {
            Finding f = d.rows().toList().get(0).findings().get(0);
            assertEquals(FindingKind.TOO_LONG, f.kind());
            assertEquals(10, f.limit().orElseThrow());
        }
    }

    /* ------------------------------------------------------------- lifecycle -- */

    @Test
    void aClosedDiffRefusesToBeUsed() {
        Diff d = tiny();
        d.close();
        d.close(); // idempotent
        assertThrows(CsvDiffException.class, d::columns);
        assertThrows(CsvDiffException.class, () -> d.emit(EmitFormat.SUMMARY));
        assertThrows(CsvDiffException.class, d::rows);
    }

    @Test
    void reportsWhatItReservedFromTheSystem() {
        try (Diff d = tiny()) {
            assertTrue(d.bytesReserved() > 0);
        }
    }

    /* -------------------------------------------------------------- options -- */

    @Test
    void movesCanBeTurnedOffAndTheSummarySaysSo() {
        try (Diff d = CsvDiff.compare(Fixtures.path("tiny_source.csv"),
                Fixtures.path("tiny_target.csv"),
                DiffOptions.builder().detectMoves(false).build())) {
            assertEquals(0, d.summary().rows().moved());
            assertTrue(d.rows().noneMatch(DiffRow::moved));
        }
    }

    @Test
    void unorderedSourcesForceMovesOffRatherThanIgnoringTheRequest() {
        try (Diff d = CsvDiff.compare(Fixtures.path("tiny_source.csv"),
                Fixtures.path("tiny_target.csv"),
                DiffOptions.builder().sourceOrdered(false).build())) {
            assertTrue(d.summary().matching().movesForcedOff());
        }
    }

    @Test
    void validationCanBeTurnedOffAndTheSummaryDistinguishesThatFromNoFindings() {
        try (Diff d = CsvDiff.compare(Fixtures.path("tiny_source.csv"),
                Fixtures.path("tiny_target.csv"),
                DiffOptions.builder().validate(false).build())) {
            assertFalse(d.summary().findings().enabled());
            assertEquals(0, d.summary().findings().total());
        }
    }

    @Test
    void truncationCutsAtAUtf8BoundaryAndSaysItTruncated() {
        String wide = "café-" + "x".repeat(50);
        byte[] source = utf8(HEADER_4 + "A,alice,1\n");
        byte[] target = utf8(HEADER_4 + "A," + wide + ",1\n");

        try (Diff d = CsvDiff.compare(DiffSource.ofBytes(source), DiffSource.ofBytes(target))) {
            // 4 bytes lands inside the e-acute, so the cut must back off to 3.
            DiffCell cell = d.rows(RowOptions.defaults().withMaxCellBytes(4)).toList()
                    .get(0).cells().get(1);
            assertEquals("caf", cell.target().orElseThrow());
            assertTrue(cell.truncated());
        }
    }
}
