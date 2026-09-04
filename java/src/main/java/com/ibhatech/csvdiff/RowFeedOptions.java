package com.ibhatech.csvdiff;

/**
 * How a row source hands its rows to the engine.
 *
 * <p>{@code batchRows} is the one that matters, and 1,000 is the figure spec 13.6
 * settled on. It is not a memory knob: the staging buffer is a fixed size and a
 * batch flushes early when it fills. It is how many rows are encoded per crossing
 * of the JNI boundary, and the reason it exists is that a crossing per row, let
 * alone per field, would cost more than the diff it is feeding.
 *
 * @param batchRows rows encoded per call into the engine
 * @param delimiter the field separator these rows are encoded with. It is not the
 *                  caller's data and never appears in the report; it only has to
 *                  match what the engine is told to parse
 * @param quote     likewise
 */
public record RowFeedOptions(int batchRows, byte delimiter, byte quote) {

    /**
     * Rejects a batch size below one.
     */
    public RowFeedOptions {
        if (batchRows < 1) throw new CsvDiffException("batchRows must be at least 1");
    }

    private static final RowFeedOptions DEFAULTS = new RowFeedOptions(1000, (byte) ',', (byte) '"');

    /**
     * A thousand rows a batch, comma delimited, double quoted.
     *
     * @return the default settings
     */
    public static RowFeedOptions defaults() {
        return DEFAULTS;
    }

    /**
     * How many rows to encode before crossing into the engine.
     *
     * @param rows the batch size, at least one
     * @return a copy with that setting
     */
    public RowFeedOptions withBatchRows(int rows) {
        return new RowFeedOptions(rows, delimiter, quote);
    }
}
