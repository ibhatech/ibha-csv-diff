package com.ibhatech.csvdiff;

/**
 * A validation finding, per spec 13.5.
 *
 * <p>These are output, not errors. A {@code REQUIRED} column with an empty cell or
 * a value over its {@code VARCHAR(n)} is <em>the point</em> of running the
 * comparison, so finding one never aborts the diff and never causes a row to be
 * dropped by changes-only. Aborting on the first would hide the other four hundred
 * and make the feature useless.
 *
 * <p>They are evaluated against the source file's schema, which spec 13.8 makes
 * authoritative, and on the values the report row actually carries: the target row
 * where there is one, the source row for a deleted row.
 */
public enum FindingKind {
    /** A {@code REQUIRED} column with an empty value. */
    REQUIRED_EMPTY("requiredEmpty", 0x04),
    /** Longer than its {@code VARCHAR(n)} or {@code CHAR(n)}. */
    TOO_LONG("tooLong", 0x08),
    /** Does not parse as its {@code DECIMAL} or {@code INTEGER}. */
    NOT_NUMERIC("notNumeric", 0x10),
    /** Parses, but exceeds its {@code DECIMAL(p,s)}. */
    PRECISION("precision", 0x20);

    private final String wireName;
    private final int bit;

    FindingKind(String wireName, int bit) {
        this.wireName = wireName;
        this.bit = bit;
    }

    /**
     * The name this finding carries in the emitters' output.
     *
     * @return the wire name
     */
    public String wireName() {
        return wireName;
    }

    /** The {@code IBHA_CSVD_CELL_*} bit that reports it. Part of the surface a
     * backend outside this package uses to build rows.
     *
     * @return the bit that reports this finding
     */
    public int bit() {
        return bit;
    }
}
