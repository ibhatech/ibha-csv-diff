package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.nio.charset.StandardCharsets;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Intra cell segments, per spec 7, which is the "Accident violation code" becoming
 * "Accident Violation code(s)" case: highlighting the whole cell loses exactly the
 * information the reviewer needs.
 */
class CellSegmentsTest {

    private static final String HEADER = """
            KEY,
            REQUIRED,
            VARCHAR(10),VARCHAR(80)
            id,text
            """;

    private static byte[] pair(String value) {
        return (HEADER + "A," + value + "\n").getBytes(StandardCharsets.UTF_8);
    }

    @Test
    void wordSegmentsCoverTheValueAndReconstructIt() {
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(pair("\"Accident violation code\"")),
                DiffSource.ofBytes(pair("\"Accident Violation code(s)\"")))) {

            DiffRow row = d.rows().toList().get(0);
            List<TextSegment> segments = d.cellSegments(row, 1, CellDiffMode.WORD);
            assertTrue(segments.size() > 1, "expected the cell to be split up");

            byte[] source = "Accident violation code".getBytes(StandardCharsets.UTF_8);
            byte[] target = "Accident Violation code(s)".getBytes(StandardCharsets.UTF_8);

            StringBuilder fromSource = new StringBuilder();
            StringBuilder fromTarget = new StringBuilder();
            for (TextSegment s : segments) {
                switch (s.op()) {
                    case EQUAL -> {
                        fromSource.append(s.applyTo(source));
                        fromTarget.append(s.applyTo(source));
                    }
                    case DELETE -> fromSource.append(s.applyTo(source));
                    case INSERT -> fromTarget.append(s.applyTo(target));
                }
            }
            assertEquals("Accident violation code", fromSource.toString());
            assertEquals("Accident Violation code(s)", fromTarget.toString());
        }
    }

    @Test
    void offsetsAreByteOffsetsAndSurviveNonAscii() {
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(pair("café noir")),
                DiffSource.ofBytes(pair("café blanc")))) {

            DiffRow row = d.rows().toList().get(0);
            byte[] source = "café noir".getBytes(StandardCharsets.UTF_8);
            byte[] target = "café blanc".getBytes(StandardCharsets.UTF_8);

            StringBuilder rebuilt = new StringBuilder();
            for (TextSegment s : d.cellSegments(row, 1, CellDiffMode.WORD_THEN_CHARACTER)) {
                if (s.op() != TextSegment.Op.DELETE) {
                    rebuilt.append(s.applyTo(s.op() == TextSegment.Op.INSERT ? target : source));
                }
            }
            assertEquals("café blanc", rebuilt.toString());
        }
    }

    @Test
    void anUnchangedOrAbsentCellHasNoSegments() {
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(pair("same")),
                DiffSource.ofBytes(pair("same")))) {
            DiffRow row = d.rows().toList().get(0);
            assertEquals(List.of(), d.cellSegments(row, 1, CellDiffMode.WORD));
            assertEquals(List.of(), d.cellSegments(row, 1, CellDiffMode.NONE));
        }
    }

    /** A row the cursor has already walked past still answers, because the engine
     *  reads only the two row indices off it. A reviewer clicks a cell long after
     *  the walk finished. */
    @Test
    void segmentsWorkOnARowTheCursorHasMovedPast() {
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(pair("one two")),
                DiffSource.ofBytes(pair("one three")))) {
            List<DiffRow> rows = d.rows().toList();
            d.rows().count(); // drain another cursor over the top
            assertTrue(d.cellSegments(rows.get(0), 1, CellDiffMode.WORD).size() > 1);
        }
    }
}
