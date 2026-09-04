package com.ibhatech.csvdiff;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * The header rows that accompany a row source.
 *
 * <p><strong>Why this is a separate object and not read off the rows.</strong> A
 * JDBC {@code ResultSet} is data rows only. The engine's model wants up to four
 * header rows above the data: {@code KEY} markers, {@code REQUIRED} markers,
 * declared types, and column names. Nothing in a result set carries the first, and
 * where the rest live, a metadata table, a config file, a schema registry, or
 * {@code ResultSetMetaData}, differs per deployment and is the caller's business.
 * So the caller supplies the header and this binding derives none of it.
 *
 * <p>The header is written into the byte stream ahead of the first data row, so the
 * engine sees one ordinary CSV with a header. There is no second parser and no
 * second code path: a {@code ResultSet} and a file reach the same state machine.
 *
 * <h2>Which side you are describing</h2>
 *
 * <p>The <strong>source</strong> is schema-authoritative, per spec 13.8, so give it
 * the whole header: keys, required flags and types. The <strong>target</strong>
 * inherits all of that from the source, so {@link #namesOnly(List)} is enough
 * there, and the names must equal the source's names because that is what header
 * detection matches on.
 *
 * <h2>Three ways to build one</h2>
 *
 * {@snippet :
 * // stated by the caller, the normal source side case
 * Header h = Header.builder()
 *     .column("account_id", "VARCHAR(20)").key().required()
 *     .column("period",     "CHAR(7)").key().required()
 *     .column("premium",    "DECIMAL(12,2)")
 *     .build();
 *
 * // a target side result set, where everything but the names is inherited
 * Header t = Header.namesOnly(List.of("account_id", "period", "premium"));
 *
 * // four rows the caller already holds, in the canonical order
 * Header r = Header.ofRows(List.of(
 *     List.of("KEY", "KEY", ""),
 *     List.of("REQUIRED", "REQUIRED", ""),
 *     List.of("VARCHAR(20)", "CHAR(7)", "DECIMAL(12,2)"),
 *     List.of("account_id", "period", "premium")));
 * }
 */
public final class Header {

    private final List<Column> columns;
    private final boolean namesOnly;

    private Header(List<Column> columns, boolean namesOnly) {
        if (columns.isEmpty()) throw new CsvDiffException("a header must declare at least one column");
        this.columns = List.copyOf(columns);
        this.namesOnly = namesOnly;
    }

    /**
     * A four row header: key markers, required markers, types, names.
     *
     * @param columns the columns, in file order
     * @return the header
     */
    public static Header of(List<Column> columns) {
        return new Header(columns, false);
    }

    /**
     * A four row header.
     *
     * @param columns the columns, in file order
     * @return the header
     */
    public static Header of(Column... columns) {
        return new Header(Arrays.asList(columns), false);
    }

    /**
     * A one row header carrying names alone.
     *
     * <p>This is the target side of a comparison: spec 13.8 makes the source
     * authoritative for keys, required flags and types, and the target inherits
     * them. Using it for a source instead means a diff with no key columns, which
     * takes the all-keys similarity path of spec 6.4 rather than keyed matching.
     *
     * @param names the column names, in file order
     * @return the header, which writes one row
     */
    public static Header namesOnly(List<String> names) {
        List<Column> cols = new ArrayList<>(names.size());
        for (String n : names) cols.add(Column.of(n));
        return new Header(cols, true);
    }

    /**
     * A header from rows the caller already holds, in the canonical order: key
     * markers, required markers, types, names.
     *
     * <p>Accepts exactly four rows, or one row which is taken as names only. A
     * marker cell counts when it reads {@code KEY} or {@code REQUIRED} in any case,
     * which is the same rule the engine applies to a file.
     *
     * @param rows exactly four rows in the canonical order, or one row of names
     * @return the header
     */
    public static Header ofRows(List<? extends List<String>> rows) {
        if (rows.size() == 1) return namesOnly(rows.get(0));
        if (rows.size() != 4) {
            throw new CsvDiffException(
                    "a header must be four rows (key, required, type, name) or one row of names, received "
                            + rows.size());
        }
        List<String> keys = rows.get(0);
        List<String> required = rows.get(1);
        List<String> types = rows.get(2);
        List<String> names = rows.get(3);

        List<Column> cols = new ArrayList<>(names.size());
        for (int i = 0; i < names.size(); i++) {
            cols.add(new Column(names.get(i), at(types, i), isMarker(at(keys, i), "KEY"),
                    isMarker(at(required, i), "REQUIRED")));
        }
        return new Header(cols, false);
    }

    private static String at(List<String> row, int i) {
        return i < row.size() && row.get(i) != null ? row.get(i) : "";
    }

    private static boolean isMarker(String cell, String marker) {
        return cell.trim().equalsIgnoreCase(marker);
    }

    /**
     * A builder, for naming the columns one at a time.
     *
     * @return an empty builder
     */
    public static Builder builder() {
        return new Builder();
    }

    /**
     * The columns this header declares.
     *
     * @return the columns, in file order
     */
    public List<Column> columns() {
        return columns;
    }

    /**
     * How many columns this header declares.
     *
     * @return the column count
     */
    public int columnCount() {
        return columns.size();
    }

    /**
     * The column names, in order.
     *
     * @return the names
     */
    public List<String> names() {
        return columns.stream().map(Column::name).toList();
    }

    /**
     * How many header rows this writes: 4, or 1 for a names-only header.
     *
     * @return 4, or 1 for a names-only header
     */
    public int rowCount() {
        return namesOnly ? 1 : 4;
    }

    boolean isNamesOnly() {
        return namesOnly;
    }

    /**
     * The header rows, in the order they are written.
     *
     * <p>Public because it is exactly what a caller needs in order to write the same
     * header into a CSV file that this binding will later compare against a result
     * set, and having them build that by hand is how the two drift apart.
     *
     * @return the header rows, in the canonical order
     */
    public List<List<String>> toRows() {
        if (namesOnly) return List.of(names());
        List<String> keys = new ArrayList<>(columns.size());
        List<String> required = new ArrayList<>(columns.size());
        List<String> types = new ArrayList<>(columns.size());
        for (Column c : columns) {
            keys.add(c.key() ? "KEY" : "");
            required.add(c.required() ? "REQUIRED" : "");
            types.add(c.type());
        }
        return List.of(keys, required, types, names());
    }

    /** Builds a header column by column. {@link #key()} and {@link #required()}
     *  apply to the column most recently added, which reads the way the header
     *  itself does. */
    public static final class Builder {
        private final List<Column> columns = new ArrayList<>();

        private Builder() {
        }

        /**
         * Adds a column with no declared type.
         *
         * @param name the column name
         * @return this builder
         */
        public Builder column(String name) {
            columns.add(Column.of(name));
            return this;
        }

        /**
         * Adds a column with a declared type.
         *
         * @param name the column name
         * @param type the declared type, as the header's type row spells it
         * @return this builder
         */
        public Builder column(String name, String type) {
            columns.add(Column.of(name, type));
            return this;
        }

        /**
         * Adds a column the caller already built.
         *
         * @param column the column
         * @return this builder
         */
        public Builder column(Column column) {
            columns.add(column);
            return this;
        }

        /**
         * Marks the column just added as part of the row key.
         *
         * @return this builder
         */
        public Builder key() {
            return replaceLast(last().asKey(), "key()");
        }

        /**
         * Marks the column just added as required.
         *
         * @return this builder
         */
        public Builder required() {
            return replaceLast(last().asRequired(), "required()");
        }

        /**
         * Marks the named columns as part of the row key, for a caller that holds
         * the key list separately from the column list.
         *
         * @param names the names of the columns to mark
         * @return this builder
         */
        public Builder keys(Collection<String> names) {
            Set<String> want = new LinkedHashSet<>(names);
            for (int i = 0; i < columns.size(); i++) {
                if (want.remove(columns.get(i).name())) columns.set(i, columns.get(i).asKey());
            }
            if (!want.isEmpty()) {
                throw new CsvDiffException("no such column to mark as a key: " + want);
            }
            return this;
        }

        /**
         * As {@link #keys(Collection)}, for names given inline.
         *
         * @param names the names of the columns to mark
         * @return this builder
         */
        public Builder keys(String... names) {
            return keys(Arrays.asList(names));
        }

        private Column last() {
            if (columns.isEmpty()) {
                throw new CsvDiffException("add a column before marking it");
            }
            return columns.get(columns.size() - 1);
        }

        private Builder replaceLast(Column c, String what) {
            if (columns.isEmpty()) throw new CsvDiffException(what + " needs a column first");
            columns.set(columns.size() - 1, c);
            return this;
        }

        /**
         * The header as named.
         *
         * @return an immutable {@link Header}
         */
        public Header build() {
            return Header.of(columns);
        }
    }
}
