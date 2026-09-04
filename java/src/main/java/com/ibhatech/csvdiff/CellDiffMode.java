package com.ibhatech.csvdiff;

/**
 * How far inside a changed cell to diff, per spec 7.
 *
 * <p>Highlighting a whole cell loses the information a reviewer needs when
 * "Accident violation code" becomes "Accident Violation code(s)". This is computed
 * per cell on request and never in bulk.
 *
 * <p>The ordinals are the engine's {@code ibha_csvd_cell_diff_mode}.
 */
public enum CellDiffMode {
    /** The cell is changed or it is not. */
    NONE,
    /** Tokens split on whitespace and punctuation. */
    WORD,
    /** Bytes, never splitting a UTF-8 sequence. */
    CHARACTER,
    /** Word first, then a character refinement inside each replaced token run.
     *  This is what GitHub's intra line highlighting does and it reads better than
     *  either level alone. */
    WORD_THEN_CHARACTER
}
