package com.ibhatech.csvdiff;

import java.util.Optional;
import java.util.regex.Pattern;

/**
 * How an emitter writes the report.
 *
 * @param changesOnly     skip rows that are unchanged, unmoved and carry neither a
 *                        suppressed cell nor a finding. The usual choice for an
 *                        HTML report
 * @param includeValues   emit cell values. Off gives the edit script alone
 * @param cellDiff        HTML only: highlight inside a changed cell, per spec 7.
 *                        The other formats carry values, not markup
 * @param maxCellBytes    truncate an emitted value at this many bytes, backing off
 *                        to a UTF-8 boundary, and mark the cell truncated
 * @param maxRows         stop after this many report rows. This is how HTML output
 *                        is kept bounded, which it must be
 * @param csvFormulaGuard CSV only. A value opening with {@code =}, {@code +},
 *                        {@code @}, a tab or a CR is a formula to Excel, so a diff
 *                        report of untrusted data is a script delivery mechanism
 *                        unless something intervenes. On by default; such a value
 *                        is prefixed with a single quote, Excel's own text marker
 * @param csvDelimiter    CSV only. The output delimiter
 * @param classPrefix     HTML only. Prefix of every emitted class name
 */
public record EmitOptions(boolean changesOnly, boolean includeValues, CellDiffMode cellDiff,
                          int maxCellBytes, int maxRows, boolean csvFormulaGuard, byte csvDelimiter,
                          Optional<String> classPrefix) {

    /**
     * The engine's own pattern, restated.
     *
     * <p>It is checked here rather than left to the engine, which would be the
     * simpler code and the worse behaviour. The engine's context holds one error and
     * the first one wins, so an engine level refusal of a mistyped class prefix
     * would abort a comparison that was otherwise complete and correct, and every
     * later call on the same handle. A caller's typo should cost the caller the
     * call, not the diff.
     */
    private static final Pattern PREFIX = Pattern.compile("[A-Za-z][A-Za-z0-9_-]{0,31}");

    /**
     * Rejects a negative limit and an HTML class prefix that is not a safe token.
     */
    public EmitOptions {
        if (maxCellBytes < 0 || maxRows < 0) {
            throw new CsvDiffException("maxCellBytes and maxRows must not be negative");
        }
        classPrefix.ifPresent(p -> {
            if (!PREFIX.matcher(p).matches()) {
                throw new CsvDiffException(
                        "the HTML class prefix must match [A-Za-z][A-Za-z0-9_-]{0,31}, received \""
                                + p + "\"");
            }
        });
    }

    private static final EmitOptions DEFAULTS = new EmitOptions(false, true, CellDiffMode.NONE, 0, 0,
            true, (byte) ',', Optional.empty());

    /**
     * Every row, with values, no cell diff.
     *
     * @return the default settings
     */
    public static EmitOptions defaults() {
        return DEFAULTS;
    }

    /**
     * The usual choice for a report: only the rows with something to say.
     *
     * @return the defaults with changes-only on
     */
    public static EmitOptions changes() {
        return DEFAULTS.withChangesOnly(true);
    }

    /**
     * Drop rows that are unchanged, unmoved and carry no finding.
     *
     * @param v true to drop the quiet rows
     * @return a copy with that setting
     */
    public EmitOptions withChangesOnly(boolean v) {
        return new EmitOptions(v, includeValues, cellDiff, maxCellBytes, maxRows, csvFormulaGuard,
                csvDelimiter, classPrefix);
    }

    /**
     * Include cell values, not just the counts and kinds.
     *
     * @param v true to include values
     * @return a copy with that setting
     */
    public EmitOptions withIncludeValues(boolean v) {
        return new EmitOptions(changesOnly, v, cellDiff, maxCellBytes, maxRows, csvFormulaGuard,
                csvDelimiter, classPrefix);
    }

    /**
     * How finely to segment a changed cell, per spec 7.
     *
     * @param v the segmentation mode
     * @return a copy with that setting
     */
    public EmitOptions withCellDiff(CellDiffMode v) {
        return new EmitOptions(changesOnly, includeValues, v, maxCellBytes, maxRows, csvFormulaGuard,
                csvDelimiter, classPrefix);
    }

    /**
     * Truncate a cell value past this many UTF-8 bytes. 0 means no limit.
     *
     * @param v the ceiling in bytes
     * @return a copy with that setting
     */
    public EmitOptions withMaxCellBytes(int v) {
        return new EmitOptions(changesOnly, includeValues, cellDiff, v, maxRows, csvFormulaGuard,
                csvDelimiter, classPrefix);
    }

    /**
     * Stop after this many report rows. 0 means no limit.
     *
     * @param v the ceiling in rows
     * @return a copy with that setting
     */
    public EmitOptions withMaxRows(int v) {
        return new EmitOptions(changesOnly, includeValues, cellDiff, maxCellBytes, v,
                csvFormulaGuard, csvDelimiter, classPrefix);
    }

    /**
     * Prefix a CSV cell that would otherwise be read as a formula by a spreadsheet.
     *
     * @param v true to guard against formula injection
     * @return a copy with that setting
     */
    public EmitOptions withCsvFormulaGuard(boolean v) {
        return new EmitOptions(changesOnly, includeValues, cellDiff, maxCellBytes, maxRows, v,
                csvDelimiter, classPrefix);
    }

    /**
     * The delimiter the CSV emitter writes.
     *
     * @param c the delimiter, which must be one byte
     * @return a copy with that setting
     */
    public EmitOptions withCsvDelimiter(char c) {
        if (c > 0x7f) throw new CsvDiffException("the CSV delimiter must be a single byte");
        return new EmitOptions(changesOnly, includeValues, cellDiff, maxCellBytes, maxRows,
                csvFormulaGuard, (byte) c, classPrefix);
    }

    /**
     * Namespace the HTML emitter's class names, so they cannot collide with the
     * host page's stylesheet.
     *
     * @param prefix the prefix, matching {@code [A-Za-z][A-Za-z0-9_-]{0,31}}
     * @return a copy with that setting
     */
    public EmitOptions withClassPrefix(String prefix) {
        return new EmitOptions(changesOnly, includeValues, cellDiff, maxCellBytes, maxRows,
                csvFormulaGuard, csvDelimiter, Optional.of(prefix));
    }
}
