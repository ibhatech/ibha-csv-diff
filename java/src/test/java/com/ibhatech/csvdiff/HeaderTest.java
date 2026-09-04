package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The header the caller supplies alongside a row source.
 *
 * <p>The three construction paths must produce the same rows, because they are the
 * same header expressed three ways and a caller who switches between them should
 * not get a different diff.
 */
class HeaderTest {

    @Test
    void theBuilderAndTheRawRowsAgree() {
        Header built = Header.builder()
                .column("account_id", "VARCHAR(20)").key().required()
                .column("period", "CHAR(7)").key()
                .column("premium", "DECIMAL(12,2)")
                .build();

        Header raw = Header.ofRows(List.of(
                List.of("KEY", "KEY", ""),
                List.of("REQUIRED", "", ""),
                List.of("VARCHAR(20)", "CHAR(7)", "DECIMAL(12,2)"),
                List.of("account_id", "period", "premium")));

        assertEquals(built.toRows(), raw.toRows());
        assertEquals(built.columns(), raw.columns());
    }

    @Test
    void theRowsAreWrittenInTheOrderTheEngineExpects() {
        Header h = Header.builder().column("id", "VARCHAR(4)").key().required().build();
        assertEquals(List.of(
                List.of("KEY"),
                List.of("REQUIRED"),
                List.of("VARCHAR(4)"),
                List.of("id")), h.toRows());
        assertEquals(4, h.rowCount());
    }

    @Test
    void markersAreMatchedCaseInsensitivelyLikeTheEngineDoes() {
        Header h = Header.ofRows(List.of(
                List.of("key", " Key ", ""),
                List.of("required", "", ""),
                List.of("", "", ""),
                List.of("a", "b", "c")));
        assertTrue(h.columns().get(0).key());
        assertTrue(h.columns().get(1).key());
        assertTrue(h.columns().get(0).required());
    }

    @Test
    void namesOnlyIsOneRowAndDeclaresNothingElse() {
        Header h = Header.namesOnly(List.of("a", "b"));
        assertEquals(1, h.rowCount());
        assertEquals(List.of(List.of("a", "b")), h.toRows());
        assertTrue(h.columns().stream().noneMatch(Column::key));
    }

    @Test
    void keysCanBeMarkedFromASeparateList() {
        Header h = Header.builder()
                .column("a").column("b").column("c")
                .keys("a", "c")
                .build();
        assertEquals(List.of(true, false, true), h.columns().stream().map(Column::key).toList());
    }

    @Test
    void namingAKeyColumnThatIsNotThereIsRefused() {
        CsvDiffException e = assertThrows(CsvDiffException.class,
                () -> Header.builder().column("a").keys("b").build());
        assertTrue(e.getMessage().contains("b"), e.getMessage());
    }

    @Test
    void aHeaderMustHaveOneOrFourRows() {
        assertThrows(CsvDiffException.class,
                () -> Header.ofRows(List.of(List.of("a"), List.of("b"))));
        assertThrows(CsvDiffException.class, () -> Header.of(List.of()));
    }

    /** A short header row is padded rather than throwing, because a caller holding
     *  four rows of unequal length has a marker row that simply stops early, which
     *  is what a spreadsheet writes. */
    @Test
    void aShortMarkerRowMeansTheRestAreUnmarked() {
        Header h = Header.ofRows(List.of(
                List.of("KEY"),
                List.of(),
                List.of("VARCHAR(4)"),
                List.of("a", "b", "c")));
        assertEquals(3, h.columnCount());
        assertTrue(h.columns().get(0).key());
        assertTrue(!h.columns().get(2).key());
        assertEquals("", h.columns().get(2).type());
    }
}
