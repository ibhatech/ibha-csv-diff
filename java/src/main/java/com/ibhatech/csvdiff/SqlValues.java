package com.ibhatech.csvdiff;

import java.io.Reader;
import java.math.BigDecimal;
import java.sql.Clob;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.OffsetDateTime;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;

/**
 * How a JDBC value becomes the text the engine compares.
 *
 * <p><strong>Why not {@code rs.getString(i)}.</strong> It is the driver's rendering,
 * and drivers disagree about exactly the types a reconciliation cares about: a
 * {@code TIMESTAMP} comes back with a space or a {@code T}, with three or six or
 * nine fractional digits or none; a {@code NUMBER} comes back as {@code 1.50} or
 * {@code 1.5} or {@code 1.5E0}; a {@code BOOLEAN} as {@code true}, {@code 1} or
 * {@code t}. The determinism check in this project requires byte identical output
 * across builds, and a diff that depends on which driver the caller happens to have
 * on the classpath is not a diff anyone can act on. So the rendering is stated here,
 * once, and it is the same on every driver.
 *
 * <p>The rules:
 *
 * <pre>
 *   SQL NULL      -&gt; an empty field
 *   BigDecimal    -&gt; toPlainString(), so 1.50 stays 1.50 and never becomes 1.5E0
 *   double, float -&gt; plain decimal, never exponent notation
 *   DATE          -&gt; 2026-01-31
 *   TIME          -&gt; 14:22:05
 *   TIMESTAMP     -&gt; 2026-01-31T14:22:05, with .SSS, .SSSSSS or .SSSSSSSSS only
 *                    when the fraction is non zero
 *   BOOLEAN       -&gt; TRUE / FALSE, which are in the engine's default truth sets
 *   CLOB          -&gt; its text
 *   BLOB, byte[]  -&gt; refused
 *   anything else -&gt; String.valueOf
 * </pre>
 *
 * <p><strong>A zero fraction is rendered away, and the engine agrees.</strong> This
 * writes {@code 2026-01-31T14:22:05} where the fraction is zero, and a column
 * declared {@code TIMESTAMP} compares that equal to {@code 2026-01-31T14:22:05.000}
 * in the file, because trailing zeros in fractional seconds are not significant
 * (spec 13.13.2). So an export written to fixed millisecond precision matches this
 * rendering rather than reporting every row of the table as modified. The two meet
 * in the middle: neither side compensates for the other. Note this is a property of
 * the declared type, exactly as {@code 1.50 == 1.5} only holds in a
 * {@code DECIMAL} column, so the same two values in a {@code VARCHAR} column are
 * text and still differ.
 *
 * <p><strong>NULL and the empty string are the same field</strong>, because CSV has
 * no NULL. Making the JDBC side distinguish them would report differences that the
 * file side could never agree with, which is precisely the disagreement between the
 * server view and the browser preview that spec section 9 warns about.
 *
 * <p>Binary is refused rather than silently base64'd or hex encoded: any encoding
 * this picked would be one the file side did not use, so every such cell would
 * report as changed. A caller who needs to compare binary should project it in SQL
 * to whatever text form the file already carries.
 */
final class SqlValues {

    private SqlValues() {
    }

    private static final DateTimeFormatter TIME = DateTimeFormatter.ofPattern("HH:mm:ss");
    private static final DateTimeFormatter DATE_TIME =
            DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss");

    /** Renders column {@code i} (1 based, as JDBC counts) of the current row. */
    static String render(ResultSet rs, int i) throws SQLException {
        Object v = rs.getObject(i);
        if (v == null || rs.wasNull()) return "";
        return render(v, i);
    }

    static String render(Object v, int column) throws SQLException {
        if (v == null) return "";

        if (v instanceof String s) return s;
        if (v instanceof BigDecimal d) return d.toPlainString();
        if (v instanceof Boolean b) return b ? "TRUE" : "FALSE";

        if (v instanceof java.sql.Timestamp t) return timestamp(t.toLocalDateTime());
        if (v instanceof java.sql.Date d) return d.toLocalDate().toString();
        if (v instanceof java.sql.Time t) return t.toLocalTime().format(TIME);

        if (v instanceof LocalDate d) return d.toString();
        if (v instanceof LocalDateTime d) return timestamp(d);
        if (v instanceof LocalTime t) return t.format(TIME);
        if (v instanceof OffsetDateTime d) return timestamp(d.toLocalDateTime());
        if (v instanceof java.time.Instant t) {
            return timestamp(LocalDateTime.ofInstant(t, ZoneOffset.UTC));
        }

        if (v instanceof Double d) return plain(d);
        if (v instanceof Float f) return plain(f.doubleValue());
        if (v instanceof Number n) return n.toString();

        if (v instanceof Clob c) return clob(c);

        if (v instanceof byte[] || v instanceof java.sql.Blob) {
            throw new CsvDiffException(
                    "column " + column + " is binary, and there is no text form of it that a CSV"
                            + " file would agree with. Project it in SQL to the text the file"
                            + " carries, or leave it out of the comparison");
        }

        return String.valueOf(v);
    }

    /**
     * Seconds always, a fraction only when there is one, and then in groups of
     * three so that a millisecond precision column never renders nine digits of
     * zeros. {@code LocalDateTime.toString} is not used because it drops the
     * seconds entirely when they are zero, which would make midnight render
     * differently from every other minute of the day.
     */
    private static String timestamp(LocalDateTime t) {
        String base = t.format(DATE_TIME);
        int nanos = t.getNano();
        if (nanos == 0) return base;
        String digits = String.format("%09d", nanos);
        int keep = 9;
        while (keep > 3 && digits.charAt(keep - 1) == '0' && digits.charAt(keep - 2) == '0'
                && digits.charAt(keep - 3) == '0') {
            keep -= 3;
        }
        return base + "." + digits.substring(0, keep);
    }

    /** Plain decimal, never exponent notation, because {@code 1.0E10} in one file
     *  and {@code 10000000000} in the other is a difference nobody made. */
    private static String plain(double d) {
        if (Double.isNaN(d) || Double.isInfinite(d)) return String.valueOf(d);
        return BigDecimal.valueOf(d).toPlainString();
    }

    private static String clob(Clob c) throws SQLException {
        long len = c.length();
        if (len > Integer.MAX_VALUE) {
            throw new CsvDiffException("a CLOB of " + len + " characters is too large to compare");
        }
        try (Reader r = c.getCharacterStream()) {
            StringBuilder sb = new StringBuilder((int) len);
            char[] buf = new char[8192];
            for (int n = r.read(buf); n >= 0; n = r.read(buf)) sb.append(buf, 0, n);
            return sb.toString();
        } catch (java.io.IOException e) {
            throw new CsvDiffException("could not read a CLOB value", e);
        }
    }
}
