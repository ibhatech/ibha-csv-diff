package com.ibhatech.csvdiff;

/**
 * The emitters the engine ships, per spec 13.3. Each is a thin loop over the same
 * cursor the streamed rows come from, so a report and a walk agree by construction.
 *
 * <p>The ordinals are the engine's {@code ibha_csvd_emit_format}.
 */
public enum EmitFormat {
    /** One JSON object per report row. The default machine format: it streams,
     *  appends, greps and splits. */
    JSONL,
    /** A CSV diff report, for loading back into a spreadsheet or a table. */
    CSV,
    /**
     * A {@code <div>} containing a {@code <table>} with the standard classes.
     *
     * <p>For <em>bounded</em> output: reports, changes-only, a page at a time. A
     * 90,000 row diff rendered to one HTML string is tens of megabytes of DOM and
     * will not scroll acceptably.
     */
    HTML,
    /** Counts, schema findings and validation findings as one JSON object, in
     *  constant memory whatever the diff size. Pass/fail checks and email bodies. */
    SUMMARY
}
