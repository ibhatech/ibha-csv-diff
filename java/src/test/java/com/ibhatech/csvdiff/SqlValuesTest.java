package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.math.BigDecimal;
import java.sql.SQLException;
import java.sql.Timestamp;
import java.time.LocalDate;
import java.time.LocalDateTime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

/**
 * The rendering rules, asserted one by one.
 *
 * <p>Each of these is a case where two drivers disagree and the diff would follow
 * whichever one the caller happened to have on the classpath. That is the whole
 * reason the rules are stated in this binding rather than delegated to
 * {@code rs.getString}.
 */
class SqlValuesTest {

    private static String render(Object v) throws SQLException {
        return SqlValues.render(v, 1);
    }

    @Test
    void nullIsAnEmptyField() throws SQLException {
        assertEquals("", render(null));
    }

    @Test
    void decimalsKeepTheirDeclaredScale() throws SQLException {
        assertEquals("1.50", render(new BigDecimal("1.50")));
        assertEquals("0.001", render(new BigDecimal("0.001")));
        assertEquals("10000000000", render(new BigDecimal("1.0E+10")));
    }

    @Test
    void doublesNeverRenderInExponentNotation() throws SQLException {
        assertEquals("10000000000", render(1.0E10));
        assertEquals("0.5", render(0.5));
    }

    @Test
    void timestampsCarrySecondsAlwaysAndAFractionOnlyWhenThereIsOne() throws SQLException {
        assertEquals("2026-01-31T14:22:05", render(Timestamp.valueOf("2026-01-31 14:22:05")));
        // midnight is where LocalDateTime.toString would have dropped the seconds
        assertEquals("2026-01-31T00:00:00", render(Timestamp.valueOf("2026-01-31 00:00:00")));
        assertEquals("2026-01-31T14:22:05.123",
                render(Timestamp.valueOf("2026-01-31 14:22:05.123")));
        assertEquals("2026-01-31T14:22:05.123456",
                render(LocalDateTime.parse("2026-01-31T14:22:05.123456")));
    }

    @Test
    void datesAreIso8601() throws SQLException {
        assertEquals("2026-01-31", render(java.sql.Date.valueOf("2026-01-31")));
        assertEquals("2026-01-31", render(LocalDate.of(2026, 1, 31)));
    }

    @Test
    void booleansUseTheEnginesDefaultTruthWords() throws SQLException {
        assertEquals("TRUE", render(Boolean.TRUE));
        assertEquals("FALSE", render(Boolean.FALSE));
    }

    @Test
    void binaryIsRefusedRatherThanEncoded() {
        CsvDiffException e = assertThrows(CsvDiffException.class,
                () -> render(new byte[] {1, 2, 3}));
        assertEquals(CsvDiffException.NOT_ENGINE, e.status());
    }

    @Test
    void stringsAndIntegersPassThrough() throws SQLException {
        assertEquals("hello", render("hello"));
        assertEquals("42", render(42));
        assertEquals("-7", render(-7L));
    }
}
