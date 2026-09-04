package com.ibhatech.csvdiff;

/**
 * What happened to a report row.
 *
 * <p>Moved is deliberately not one of these. A row can move and be modified in the
 * same edit, and a single enum would lose one of the two facts, so it is a flag on
 * {@link DiffRow} instead.
 *
 * <p>The ordinals are the engine's {@code ibha_csvd_row_kind} and the wire names
 * are what the JSONL and CSV emitters write. Neither is an accident of this class:
 * both are the engine's contract, restated here so a consumer can move between a
 * streamed row and a saved report without a translation layer.
 */
public enum ChangeKind {
    /** Present on both sides, every compared cell equal. */
    UNCHANGED("unchanged"),
    /** Present on both sides, at least one compared cell different. */
    MODIFIED("modified"),
    /** In the target and not in the source. */
    ADDED("added"),
    /** In the source and not in the target. */
    DELETED("deleted");

    private final String wireName;

    ChangeKind(String wireName) {
        this.wireName = wireName;
    }

    /**
     * The name this kind carries in the emitters' output.
     *
     * @return the wire name, one of {@code unchanged}, {@code modified},
     *         {@code added} or {@code deleted}
     */
    public String wireName() {
        return wireName;
    }

    private static final ChangeKind[] BY_ORDINAL = values();

    /**
     * The kind with this {@code ibha_csvd_row_kind} ordinal. Part of the surface a
     * backend outside this package uses to build rows.
     *
     * @param kind the engine's ordinal
     * @return the matching kind
     * @throws CsvDiffException if the ordinal is not one the engine defines
     */
    public static ChangeKind ofOrdinal(int kind) {
        if (kind < 0 || kind >= BY_ORDINAL.length) {
            throw new CsvDiffException("the engine reported an unknown row kind " + kind);
        }
        return BY_ORDINAL[kind];
    }
}
