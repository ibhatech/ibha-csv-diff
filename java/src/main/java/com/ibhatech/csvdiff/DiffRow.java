package com.ibhatech.csvdiff;

import java.util.List;
import java.util.OptionalInt;

/**
 * One row of the report.
 *
 * <p>Row numbers are 1 based <em>record</em> numbers. <strong>Flagged assumption,
 * unchanged since Phase 3 and not re-decided here:</strong> they count parsed
 * records rather than physical lines, so they differ from the line number when a
 * file has blank lines or multiline quoted fields. Every row number in every
 * emitter and in every binding uses the same convention.
 *
 * <p>{@link #cells()} is empty when the row was read with values off, which is what
 * a pass/fail check or a row count wants and costs nothing to produce. Its length
 * is the number of <em>compared</em> columns, which under the column policy of spec
 * 6.6 is not always the source file's column count: read the width from the row and
 * the names from {@link Diff#columns()}.
 *
 * @param kind            unchanged, modified, added or deleted
 * @param moved           outside the longest increasing subsequence, per spec 6.2
 * @param moveDistance    how far, signed, and zero when not moved
 * @param sourceRow       1 based record number in the source, absent when added
 * @param targetRow       1 based record number in the target, absent when deleted
 * @param changedCells    how many cells differ
 * @param suppressedCells how many differ in bytes but are equal once normalized
 * @param cells           the cells, or empty when values were not requested
 * @param findings        validation findings on this row, per spec 13.5
 * @param key             the key column values, or empty when values were not
 *                        requested or the schema declares no key column
 */
public record DiffRow(
        ChangeKind kind,
        boolean moved,
        int moveDistance,
        OptionalInt sourceRow,
        OptionalInt targetRow,
        int changedCells,
        int suppressedCells,
        List<DiffCell> cells,
        List<Finding> findings,
        List<String> key) {

    /** Defensive copies, so a row handed to a consumer cannot be edited under it. */
    public DiffRow {
        cells = List.copyOf(cells);
        findings = List.copyOf(findings);
        key = List.copyOf(key);
    }

    /**
     * Whether this row would be dropped by changes-only.
     *
     * <p>The same rule the emitters apply, and the reason it is not simply
     * {@code kind == UNCHANGED}: a finding on an otherwise unchanged row is the
     * point of the run, so it is never what this drops.
     *
     * @return true when changes-only would not report this row
     */
    public boolean isQuiet() {
        return kind == ChangeKind.UNCHANGED && !moved && suppressedCells == 0 && findings.isEmpty();
    }
}
