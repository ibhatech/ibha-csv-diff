package com.ibhatech.csvdiff;

/**
 * One column of a {@link Header}.
 *
 * <p><strong>{@code type} is the declared type as text</strong>, exactly as it would
 * appear in a CSV file's type row: {@code VARCHAR(20)}, {@code DECIMAL(12,2)},
 * {@code INTEGER}, {@code DATE}. It is not a Java enum, and that is deliberate. The
 * engine parses this text once per column and the parse <em>selects the
 * comparator</em>, which is what stops a spreadsheet round trip that rewrote 1.50 as
 * 1.5 and 00123 as 123 from reporting a whole file as changed. Re-deriving it in
 * Java would be a second implementation of the one thing this binding exists not to
 * duplicate, and the two would eventually disagree about a type only one of them
 * had heard of.
 *
 * <p>An empty or unrecognized type is {@code UNKNOWN} to the engine, which compares
 * as trimmed bytes and is always safe. Anything longer than 40 characters is not
 * examined.
 *
 * @param name     the column name. On a target side these must equal the source's
 *                 names, which is what header auto-detection matches on
 * @param type     the declared type as text, or empty for none
 * @param key      part of the row key. Row matching is built on this, and nothing
 *                 in a {@code ResultSet} can tell you which columns it is
 * @param required a value is expected. An empty one becomes a finding, not an error
 */
public record Column(String name, String type, boolean key, boolean required) {

    /**
     * Rejects a null name, and treats a null type as no declared type.
     */
    public Column {
        if (name == null) throw new CsvDiffException("a column name must not be null");
        if (type == null) type = "";
    }

    /**
     * A column with no declared type, not a key and not required.
     *
     * @param name the column name
     * @return the column
     */
    public static Column of(String name) {
        return new Column(name, "", false, false);
    }

    /**
     * A column with a declared type, not a key and not required.
     *
     * @param name the column name
     * @param type the declared type, as the header's type row spells it
     * @return the column
     */
    public static Column of(String name, String type) {
        return new Column(name, type, false, false);
    }

    /**
     * This column, marked as part of the key.
     *
     * @return a copy with {@code key} set
     */
    public Column asKey() {
        return new Column(name, type, true, required);
    }

    /**
     * This column, marked as requiring a value.
     *
     * @return a copy with {@code required} set
     */
    public Column asRequired() {
        return new Column(name, type, key, true);
    }
}
