package com.ibhatech.csvdiff;

import java.util.Optional;

/**
 * One cell of one report row.
 *
 * <p><strong>The one rule a consumer gets wrong.</strong> A matched row's cell
 * carries {@link #source()} <em>exactly when that cell differs in bytes from the
 * target</em>. An empty {@code source} means the two sides are byte identical; it
 * never means the value was empty. This is what keeps an unchanged 90,000 row
 * report from being written out twice, and a consumer that reads an absent source
 * as an empty string renders every unchanged cell as a deletion.
 *
 * <p>{@code target} is absent only when the row has no target side at all, which is
 * a deleted row.
 *
 * @param column      index within the compared columns, which under the column
 *                    policy of spec 6.6 is not always the source file's column index
 * @param name        the column's name
 * @param changed     the two values differ under the column's comparator
 * @param suppressed  the two values differ in bytes but are equal once normalized,
 *                    which spec 5.3 requires be counted rather than silently hidden
 * @param source      the source value, present only under the rule above
 * @param target      the target value, absent on a deleted row
 * @param truncated   the value was cut at the caller's {@code maxCellBytes}
 * @param invalidUtf8 the bytes were not valid UTF-8 and were decoded with
 *                    replacement characters
 */
public record DiffCell(
        int column,
        String name,
        boolean changed,
        boolean suppressed,
        Optional<String> source,
        Optional<String> target,
        boolean truncated,
        boolean invalidUtf8) {
}
