package com.ibhatech.csvdiff;

/**
 * Which rows of one side are header rows, and what each of them means.
 *
 * <p>Backend surface, per the note in {@code java/README.md}: a backend outside this
 * package needs these five numbers to open a parse, and they are resolved here
 * rather than in the backend so that every backend resolves them the same way.
 *
 * <p>The row positions are 1 based within the header, and 0 means the row is absent.
 * They are derived from {@code rows} unless the caller stated them, on the same rule
 * the JavaScript binding uses, so the two cannot disagree about a file.
 *
 * @param rows        header rows on this side: 4, 1 for names only, 0 for none, or
 *                    {@link #AUTO} for the target side
 * @param keyRow      position of the {@code KEY} marker row
 * @param requiredRow position of the {@code REQUIRED} marker row
 * @param typeRow     position of the declared type row
 * @param nameRow     position of the column name row
 */
public record HeaderLayout(int rows, int keyRow, int requiredRow, int typeRow, int nameRow) {

    /**
     * {@code IBHA_CSVD_HEADER_AUTO}, which is {@code 0xFFFFFFFF} and arrives as -1:
     * detect the count against the source.
     *
     * <p>This is the target side's layout and always its layout, per spec 13.8. The
     * source is authoritative and the target's count is detected rather than guessed,
     * because we already know what the column names are, and because a sales user may
     * have uploaded a file that kept only the name row.
     */
    public static final int AUTO = -1;

    /** The target side, whose header is detected rather than declared. */
    public static final HeaderLayout AUTO_DETECT = new HeaderLayout(AUTO, 0, 0, 0, 0);

    /**
     * The layout for a header of {@code rows} rows. A negative position means the
     * caller did not state it and it is derived; a stated one is taken as given.
     *
     * <p>One copy of the derivation, so that the two callers in {@link DiffOptions},
     * the options alone and the options reconciled with a row source, cannot drift
     * apart.
     *
     * @param rows the rows, pulled one at a time
     * @param requiredRow
     * @param nameRow
     */
    static HeaderLayout derive(int rows, int keyRow, int requiredRow, int typeRow, int nameRow) {
        return new HeaderLayout(rows,
                keyRow >= 0 ? keyRow : (rows >= 1 ? 1 : 0),
                requiredRow >= 0 ? requiredRow : (rows >= 2 ? 2 : 0),
                typeRow >= 0 ? typeRow : (rows >= 3 ? 3 : 0),
                nameRow >= 0 ? nameRow : rows);
    }
}
