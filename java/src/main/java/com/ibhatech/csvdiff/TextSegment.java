package com.ibhatech.csvdiff;

/**
 * One piece of an intra cell diff, per spec 7.
 *
 * <p>Offsets are into the <em>logical</em>, unescaped value: {@link Op#EQUAL} and
 * {@link Op#DELETE} index the source value, {@link Op#INSERT} indexes the target
 * value.
 *
 * <p><strong>They are byte offsets, not character indices.</strong> A Java consumer
 * that hands them to {@link String#substring} on a string containing anything
 * outside ASCII will cut in the wrong place, because Java indexes UTF-16 code
 * units. Use {@link #applyTo(byte[])} or work on the UTF-8 bytes. Getting this
 * boundary wrong is the classic source of mangled non-ASCII text, so the unit is
 * stated here rather than implied.
 *
 * @param op    what happened to this piece
 * @param start byte offset into the logical value
 * @param len   length in bytes
 */
public record TextSegment(Op op, int start, int len) {

    /** What happened to a piece of a cell's text. */
    public enum Op {
        /** Present in both values. */
        EQUAL,
        /** Present in the source value only. */
        DELETE,
        /** Present in the target value only. */
        INSERT;

        private static final Op[] BY_ORDINAL = values();

        /**
         * The op with this {@code ibha_csvd_seg_op} ordinal.
         *
         * @param op the engine's ordinal
         * @return the matching op
         * @throws CsvDiffException if the ordinal is not one the engine defines
         */
        public static Op ofOrdinal(int op) {
            if (op < 0 || op >= BY_ORDINAL.length) {
                throw new CsvDiffException("the engine reported an unknown segment op " + op);
            }
            return BY_ORDINAL[op];
        }
    }

    /**
     * The text of this segment, taken from the UTF-8 bytes of the value it indexes.
     *
     * @param utf8Value the whole value's UTF-8 bytes, which this segment indexes into
     * @return this segment's text
     */
    public String applyTo(byte[] utf8Value) {
        return new String(utf8Value, start, len, java.nio.charset.StandardCharsets.UTF_8);
    }
}
