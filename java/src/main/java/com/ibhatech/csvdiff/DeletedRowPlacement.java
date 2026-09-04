package com.ibhatech.csvdiff;

/**
 * Where a deleted source row appears in the report, per spec 6.5.
 *
 * <p>The ordinals are the engine's {@code ibha_csvd_deleted_placement}.
 */
public enum DeletedRowPlacement {
    /**
     * The default. A deleted row is emitted immediately after the target position
     * of the most recently matched source row, so a row deleted from the middle of
     * the file appears in the middle of the report next to its former neighbours.
     * Consecutive deletions stay grouped in source order.
     */
    ANCHORED,
    /** All deletions in a block at the end. Also what anchored degrades to when the
     *  caller says the source side has no meaningful order. */
    END
}
