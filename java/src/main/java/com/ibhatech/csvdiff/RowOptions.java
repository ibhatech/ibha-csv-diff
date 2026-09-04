package com.ibhatech.csvdiff;

/**
 * How the report is walked.
 *
 * @param includeValues decode cell values. Off gives the edit script alone, which
 *                      is what a pass/fail check or a row count wants and costs
 *                      nothing: on the p90 pair it is the difference between 0.06
 *                      and 0.30 seconds, all of it spent turning 1.7 million cells
 *                      into strings
 * @param changesOnly   skip rows that are unchanged, unmoved and carry neither a
 *                      suppressed cell nor a validation finding. A finding on an
 *                      otherwise unchanged row is the point of the run, so it is
 *                      never what this drops
 * @param maxCellBytes  truncate a decoded value at this many bytes, backing off to
 *                      a UTF-8 boundary so the result is still text. 0 decodes
 *                      values whole
 * @param batchRows     report rows drained per call into the engine
 */
public record RowOptions(boolean includeValues, boolean changesOnly, int maxCellBytes,
                         int batchRows) {

    /**
     * Rejects a batch size below one and a negative cell limit.
     */
    public RowOptions {
        if (batchRows < 1) throw new CsvDiffException("batchRows must be at least 1");
        if (maxCellBytes < 0) throw new CsvDiffException("maxCellBytes must not be negative");
    }

    private static final RowOptions DEFAULTS = new RowOptions(true, false, 0, 1000);

    /**
     * Every row, with values, drained a thousand at a time.
     *
     * @return the default settings
     */
    public static RowOptions defaults() {
        return DEFAULTS;
    }

    /**
     * Just the rows with something to say, with values.
     *
     * @return the defaults with changes-only on
     */
    public static RowOptions changes() {
        return DEFAULTS.withChangesOnly(true);
    }

    /**
     * Include cell values, not just the counts and kinds.
     *
     * @param v true to include values
     * @return a copy with that setting
     */
    public RowOptions withIncludeValues(boolean v) {
        return new RowOptions(v, changesOnly, maxCellBytes, batchRows);
    }

    /**
     * Drop rows that are unchanged, unmoved and carry no finding.
     *
     * @param v true to drop the quiet rows
     * @return a copy with that setting
     */
    public RowOptions withChangesOnly(boolean v) {
        return new RowOptions(includeValues, v, maxCellBytes, batchRows);
    }

    /**
     * Truncate a cell value past this many UTF-8 bytes. 0 means no limit.
     *
     * @param v the ceiling in bytes
     * @return a copy with that setting
     */
    public RowOptions withMaxCellBytes(int v) {
        return new RowOptions(includeValues, changesOnly, v, batchRows);
    }

    /**
     * How many report rows to drain per call into the engine. Larger trades memory for fewer crossings.
     *
     * @param v the batch size, at least one
     * @return a copy with that setting
     */
    public RowOptions withBatchRows(int v) {
        return new RowOptions(includeValues, changesOnly, maxCellBytes, v);
    }
}
