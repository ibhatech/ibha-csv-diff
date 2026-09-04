package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIf;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.Optional;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The binding's decoded rows against the JSONL emitter's own output, field for
 * field.
 *
 * <p><strong>This is the load bearing test in the Java binding</strong>, and it is
 * the third implementation of the row contract to be checked this way: the JS
 * binding has the same test against the same emitter, and the view has one against
 * the HTML emitter.
 *
 * <p>The reason it exists: this binding decodes cells itself, straight out of the
 * columnar arrays, rather than asking the engine for each one, because the public
 * header says an accessor call per cell would cost more than the diff. That
 * decision buys the throughput and costs a second implementation of the row
 * contract, and a second implementation that nothing compares against the first is
 * just a place for the two to drift. So the emitter, which is fuzzed and covered on
 * the C side, is the oracle.
 *
 * <p>What would fail here and nowhere else: an off by one in the field index
 * arithmetic that only shows on a ragged row, a source value emitted when the two
 * sides are byte identical, a row number that counts lines instead of records, a
 * truncation that splits a UTF-8 sequence.
 */
class RowContractTest {

    /** The emitter's row shape, declared here rather than imported, so this asserts
     *  against what the JSONL contract actually says and not against the binding's
     *  idea of it. */
    private record EmittedCell(String name, Optional<String> source, Optional<String> target,
                               boolean changed, boolean suppressed) {
    }

    private record EmittedRow(String kind, Long sourceRow, Long targetRow, boolean moved,
                              long moveDistance, List<EmittedCell> cells, int findings) {
    }

    @Test
    @EnabledIf("hasP90")
    void matchesTheJsonlEmitterRowForRowOnTheP90Pair() {
        try (Diff d = CsvDiff.compare(Fixtures.path("p90_source.csv"),
                Fixtures.path("p90_target.csv"))) {

            List<EmittedRow> emitted = parseJsonl(d.emit(EmitFormat.JSONL, EmitOptions.changes()));
            List<DiffRow> decoded = d.rows(RowOptions.changes()).toList();

            assertEquals(emitted.size(), decoded.size(), "report row count");
            assertTrue(decoded.size() > 100, "the p90 pair should have plenty of changed rows");

            for (int i = 0; i < decoded.size(); i++) {
                DiffRow a = decoded.get(i);
                EmittedRow b = emitted.get(i);
                String at = "row " + i;

                assertEquals(b.kind(), a.kind().wireName(), at + " kind");
                assertEquals(b.sourceRow(), boxed(a.sourceRow()), at + " sourceRow");
                assertEquals(b.targetRow(), boxed(a.targetRow()), at + " targetRow");
                assertEquals(b.moved(), a.moved(), at + " moved");
                assertEquals(b.moveDistance(), a.moveDistance(), at + " moveDistance");
                assertEquals(b.findings(), a.findings().size(), at + " finding count");

                assertEquals(b.cells().size(), a.cells().size(), at + " cell count");
                for (int c = 0; c < b.cells().size(); c++) {
                    DiffCell x = a.cells().get(c);
                    EmittedCell y = b.cells().get(c);
                    String cellAt = at + " cell " + c;
                    assertEquals(y.name(), x.name(), cellAt + " name");
                    assertEquals(y.source(), x.source(), cellAt + " source");
                    assertEquals(y.target(), x.target(), cellAt + " target");
                    assertEquals(y.changed(), x.changed(), cellAt + " changed");
                    assertEquals(y.suppressed(), x.suppressed(), cellAt + " suppressed");
                }
            }
        }
    }

    /**
     * The same comparison on the awkward fixtures, where the two implementations
     * have the most room to disagree: escaped quotes, multiline fields, CRLF, a
     * BOM, Latin-1 bytes that are not valid UTF-8, and the XSS corpus.
     */
    @Test
    void matchesTheJsonlEmitterOnTheEdgeCaseFixtures() {
        record Pair(String name, String source, int headerRows) {
        }
        List<Pair> pairs = List.of(
                new Pair("tiny", "tiny_source.csv", 4),
                new Pair("escaped", "escaped_quotes.csv", 4),
                new Pair("multiline", "multiline_quoted.csv", 4),
                new Pair("crlf", "crlf.csv", 4),
                new Pair("latin1", "latin1.csv", 1),
                new Pair("xss", "xss.csv", 1));

        int compared = 0;
        for (Pair pair : pairs) {
            byte[] source = Fixtures.bytes(pair.source());
            byte[] target = pair.name().equals("tiny")
                    ? Fixtures.bytes("tiny_target.csv")
                    : Stir.edit(source, pair.headerRows());

            DiffOptions options = DiffOptions.builder().headerRows(pair.headerRows()).build();
            try (Diff d = CsvDiff.compare(DiffSource.ofBytes(source), DiffSource.ofBytes(target),
                    options)) {

                List<EmittedRow> emitted = parseJsonl(d.emit(EmitFormat.JSONL, EmitOptions.defaults()));
                List<DiffRow> decoded = d.rows().toList();
                assertEquals(emitted.size(), decoded.size(), pair.name() + ": report row count");
                assertNotEquals(0, decoded.size(), pair.name() + ": nothing to compare");

                for (int i = 0; i < decoded.size(); i++) {
                    DiffRow a = decoded.get(i);
                    EmittedRow b = emitted.get(i);
                    assertEquals(b.kind(), a.kind().wireName(), pair.name() + " row " + i);
                    for (int c = 0; c < b.cells().size(); c++) {
                        assertEquals(b.cells().get(c).source(), a.cells().get(c).source(),
                                pair.name() + " row " + i + " cell " + c + " source");
                        assertEquals(b.cells().get(c).target(), a.cells().get(c).target(),
                                pair.name() + " row " + i + " cell " + c + " target");
                    }
                    compared += b.cells().size();
                }
            }
        }
        assertTrue(compared > 500, "expected to have compared plenty of cells, got " + compared);
    }

    /** Values off is not a different report, only a cheaper one: every row still
     *  appears, in the same order, with the same classification. */
    @Test
    void valuesOffAgreesWithValuesOnAboutEveryRow() {
        try (Diff d = CsvDiff.compare(Fixtures.path("tiny_source.csv"),
                Fixtures.path("tiny_target.csv"))) {
            List<DiffRow> withValues = d.rows().toList();
            List<DiffRow> without = d.rows(RowOptions.defaults().withIncludeValues(false)).toList();

            assertEquals(withValues.size(), without.size());
            for (int i = 0; i < withValues.size(); i++) {
                assertEquals(withValues.get(i).kind(), without.get(i).kind());
                assertEquals(withValues.get(i).sourceRow(), without.get(i).sourceRow());
                assertEquals(withValues.get(i).targetRow(), without.get(i).targetRow());
                assertEquals(withValues.get(i).changedCells(), without.get(i).changedCells());
                assertTrue(without.get(i).cells().isEmpty());
            }
        }
    }

    /** changesOnly drops exactly the quiet rows and nothing else. A finding on an
     *  otherwise unchanged row is the point of the run, so it is never dropped. */
    @Test
    void changesOnlyDropsOnlyQuietRows() {
        try (Diff d = CsvDiff.compare(Fixtures.path("tiny_source.csv"),
                Fixtures.path("tiny_target.csv"))) {
            List<DiffRow> all = d.rows().toList();
            List<DiffRow> changed = d.rows(RowOptions.changes()).toList();

            assertEquals(all.stream().filter(r -> !r.isQuiet()).toList().size(), changed.size());
            assertTrue(changed.stream().noneMatch(DiffRow::isQuiet));
            assertTrue(all.size() > changed.size(), "the tiny pair has unchanged rows");
        }
    }

    /* ----------------------------------------------------------------- utils -- */

    static boolean hasP90() {
        return Fixtures.hasP90();
    }

    private static Long boxed(java.util.OptionalInt v) {
        return v.isPresent() ? (long) v.getAsInt() : null;
    }

    private static List<EmittedRow> parseJsonl(byte[] jsonl) {
        String text = new String(jsonl, StandardCharsets.UTF_8);
        return text.lines()
                .filter(line -> !line.isEmpty())
                .map(RowContractTest::parseRow)
                .toList();
    }

    private static EmittedRow parseRow(String line) {
        Object o = Json.parse(line);
        List<EmittedCell> cells = Json.arr(o, "cells").stream()
                .map(c -> new EmittedCell(
                        Json.str(c, "name"),
                        Json.has(c, "source") ? Optional.of(Json.str(c, "source")) : Optional.empty(),
                        Json.has(c, "target") ? Optional.of(Json.str(c, "target")) : Optional.empty(),
                        Json.bool(c, "changed"),
                        Json.bool(c, "suppressed")))
                .toList();
        Long sourceRow = numOrNull(o, "sourceRow");
        Long targetRow = numOrNull(o, "targetRow");
        return new EmittedRow(Json.str(o, "kind"), sourceRow, targetRow, Json.bool(o, "moved"),
                Json.num(o, "moveDistance"), cells, Json.arr(o, "findings").size());
    }

    private static Long numOrNull(Object o, String key) {
        if (!(o instanceof Map<?, ?> m) || m.get(key) == null) return null;
        return Json.num(o, key);
    }
}
