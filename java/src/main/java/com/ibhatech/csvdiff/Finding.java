package com.ibhatech.csvdiff;

import java.util.OptionalInt;

/**
 * One validation finding on one cell of one report row.
 *
 * <p>{@code limit} is set for {@link FindingKind#TOO_LONG} and is the declared
 * {@code VARCHAR(n)} or {@code CHAR(n)} length. {@code precision} and {@code scale}
 * are set for {@link FindingKind#PRECISION} and are the declared
 * {@code DECIMAL(p,s)}. All three are absent otherwise, rather than zero, because
 * a zero limit is a thing a schema can declare.
 *
 * <p><strong>Flagged assumption, unchanged since Phase 3 and not re-decided here:</strong>
 * {@code VARCHAR(n)} is counted in characters, not bytes, so {@code café} is four
 * characters in five bytes and does not violate {@code VARCHAR(5)}.
 *
 * @param column the column index within the compared columns
 * @param name   the column's name
 * @param kind   which rule the cell failed
 * @param limit  the declared length, for {@link FindingKind#TOO_LONG}
 * @param precision the declared precision, for {@link FindingKind#PRECISION}
 * @param scale  the declared scale, for {@link FindingKind#PRECISION}
 */
public record Finding(
        int column,
        String name,
        FindingKind kind,
        OptionalInt limit,
        OptionalInt precision,
        OptionalInt scale) {

    /**
     * A finding that carries no declared limit.
     *
     * @param column the 0 based column
     * @param name   that column's name
     * @param kind   which finding this is
     * @return the finding
     */
    public static Finding of(int column, String name, FindingKind kind) {
        return new Finding(column, name, kind, OptionalInt.empty(), OptionalInt.empty(),
                OptionalInt.empty());
    }

    /**
     * A value longer than its column's declared length, counted in characters.
     *
     * @param column the 0 based column
     * @param name   that column's name
     * @param limit  the declared length the value exceeded
     * @return the finding
     */
    public static Finding tooLong(int column, String name, int limit) {
        return new Finding(column, name, FindingKind.TOO_LONG, OptionalInt.of(limit),
                OptionalInt.empty(), OptionalInt.empty());
    }

    /**
     * A value outside its column's declared precision or scale.
     *
     * @param column    the 0 based column
     * @param name      that column's name
     * @param precision the declared precision
     * @param scale     the declared scale
     * @return the finding
     */
    public static Finding precision(int column, String name, int precision, int scale) {
        return new Finding(column, name, FindingKind.PRECISION, OptionalInt.empty(),
                OptionalInt.of(precision), OptionalInt.of(scale));
    }
}
