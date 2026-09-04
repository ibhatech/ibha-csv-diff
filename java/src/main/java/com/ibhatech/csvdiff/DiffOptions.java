package com.ibhatech.csvdiff;

import java.util.Optional;

/**
 * Everything that changes what the comparison means.
 *
 * <p>The defaults are the engine's own, restated here rather than left to whichever
 * side happens to fill the struct, so that reading this file tells you what a
 * comparison with no options does. They are: comma delimited, double quoted, BOM
 * stripped; four header rows in the source and auto-detection in the target;
 * whitespace trimmed, {@code CHAR(n)} padding ignored, numbers and booleans
 * compared by value; move detection on, deleted rows anchored, validation on; and
 * a column present in one file and absent from the other is an error.
 *
 * {@snippet :
 * DiffOptions o = DiffOptions.builder()
 *     .headerRows(1)                    // a names-only source file
 *     .allowAddedColumns(true)          // a salesman appending a column is a finding
 *     .validate(false)                  // the edit script alone
 *     .build();
 * }
 */
public final class DiffOptions {

    private static final DiffOptions DEFAULTS = builder().build();

    private final byte delimiter;
    private final byte quote;
    private final boolean stripBom;

    private final int headerRows;
    private final boolean headerRowsStated;
    private final int keyRow;
    private final int requiredRow;
    private final int typeRow;
    private final int nameRow;

    private final boolean trimWhitespace;
    private final boolean charIgnorePad;
    private final boolean numeric;
    private final boolean booleans;
    private final String boolTrue;
    private final String boolFalse;
    private final boolean allowAddedColumns;
    private final boolean allowRemovedColumns;

    private final boolean detectMoves;
    private final boolean sourceOrdered;
    private final boolean requireKey;
    private final DeletedRowPlacement deletedRowPlacement;
    private final int similarityPercent;
    private final int similarityCandidates;

    private final boolean validate;
    private final boolean countSuppressed;

    private final long maxBytes;
    private final int maxRows;
    private final int maxColumns;
    private final int stagingBytes;

    private DiffOptions(Builder b) {
        this.delimiter = b.delimiter;
        this.quote = b.quote;
        this.stripBom = b.stripBom;
        this.headerRows = b.headerRows;
        this.headerRowsStated = b.headerRowsStated;
        this.keyRow = b.keyRow;
        this.requiredRow = b.requiredRow;
        this.typeRow = b.typeRow;
        this.nameRow = b.nameRow;
        this.trimWhitespace = b.trimWhitespace;
        this.charIgnorePad = b.charIgnorePad;
        this.numeric = b.numeric;
        this.booleans = b.booleans;
        this.boolTrue = b.boolTrue;
        this.boolFalse = b.boolFalse;
        this.allowAddedColumns = b.allowAddedColumns;
        this.allowRemovedColumns = b.allowRemovedColumns;
        this.detectMoves = b.detectMoves;
        this.sourceOrdered = b.sourceOrdered;
        this.requireKey = b.requireKey;
        this.deletedRowPlacement = b.deletedRowPlacement;
        this.similarityPercent = b.similarityPercent;
        this.similarityCandidates = b.similarityCandidates;
        this.validate = b.validate;
        this.countSuppressed = b.countSuppressed;
        this.maxBytes = b.maxBytes;
        this.maxRows = b.maxRows;
        this.maxColumns = b.maxColumns;
        this.stagingBytes = b.stagingBytes;
    }

    /**
     * A comparison with nothing set.
     *
     * @return the engine's own defaults, as described on this class
     */
    public static DiffOptions defaults() {
        return DEFAULTS;
    }

    /**
     * A builder holding the defaults.
     *
     * @return a new builder, holding those same defaults
     */
    public static Builder builder() {
        return new Builder();
    }

    /**
     * The byte that separates fields.
     *
     * @return the field delimiter byte, comma unless set
     */
    public byte delimiter() {
        return delimiter;
    }

    /**
     * The byte that quotes a field.
     *
     * @return the quote byte, double quote unless set
     */
    public byte quote() {
        return quote;
    }

    /**
     * Whether a leading byte order mark is dropped.
     *
     * @return whether a leading byte order mark is dropped rather than
     *         becoming part of the first column's name
     */
    public boolean stripBom() {
        return stripBom;
    }

    /**
     * How many header rows the source file carries.
     *
     * @return header rows in the source file: 4, 1 or 0
     */
    public int headerRows() {
        return headerRows;
    }

    /**
     * Whether the caller stated {@link #headerRows()} rather than taking the
     * default of 4. The difference matters only to
     * {@link #headerLayout(Header)}, which will not override a stated value.
     *
     * @return true when a count was set explicitly
     */
    public boolean headerRowsStated() {
        return headerRowsStated;
    }

    /**
     * The source side's header layout, from the options alone.
     *
     * <p>Backend surface. The target side never uses this: its layout is
     * {@link HeaderLayout#AUTO_DETECT}, per spec 13.8.
     *
     * @return the five numbers a parse of the source side opens with
     */
    public HeaderLayout headerLayout() {
        return HeaderLayout.derive(headerRows, keyRow, requiredRow, typeRow, nameRow);
    }

    /**
     * The source side's header layout, reconciled with a source that declares its
     * own header.
     *
     * <p>A row source writes {@link Header#rowCount()} header rows and the parse
     * must be told that same number. Before this existed the count came from the
     * options alone, so a {@code Header.namesOnly} row source parsed under the
     * default of four consumed its name row <em>and the first three data rows</em>
     * as a header: the fourth data row became the column names, three rows vanished,
     * and nothing reported an error. The engine cannot catch that for us, because
     * four header rows is a perfectly ordinary thing to ask for.
     *
     * <p>So a declared header wins over the default, and conflicts with a
     * <em>stated</em> {@code headerRows} rather than silently overriding it: a caller
     * who wrote both down had one of the two in mind, and guessing which is how the
     * original defect would come back.
     *
     * @param declared the source's own header, or null for a byte source
     * @return the reconciled layout
     * @throws CsvDiffException if the declared header and a stated
     *                          {@link #headerRows()} disagree
     */
    public HeaderLayout headerLayout(Header declared) {
        if (declared == null) return headerLayout();

        int rows = declared.rowCount();
        if (rows == headerRows) return headerLayout();
        if (headerRowsStated) {
            throw new CsvDiffException("the source writes " + rows + " header row"
                    + (rows == 1 ? "" : "s") + " but headerRows(" + headerRows
                    + ") was set on the options. Drop one of the two: a row source states"
                    + " its header rows through its Header, and headerRows is for a byte"
                    + " source whose header is inside its own bytes");
        }
        return HeaderLayout.derive(rows, keyRow, requiredRow, typeRow, nameRow);
    }

    /** 1 based position of the key marker row, 0 for absent. Derived from
     *  {@link #headerRows()} unless set.
     *
     * @return 1 based position of the key marker row, or 0 when absent
     */
    public int keyRow() {
        return keyRow >= 0 ? keyRow : (headerRows >= 1 ? 1 : 0);
    }

    /**
     * Where the required marker row sits in the header.
     *
     * @return 1 based position of the required marker row, or 0 when absent
     */
    public int requiredRow() {
        return requiredRow >= 0 ? requiredRow : (headerRows >= 2 ? 2 : 0);
    }

    /**
     * Where the declared type row sits in the header.
     *
     * @return 1 based position of the declared type row, or 0 when absent
     */
    public int typeRow() {
        return typeRow >= 0 ? typeRow : (headerRows >= 3 ? 3 : 0);
    }

    /**
     * Where the column name row sits in the header.
     *
     * @return 1 based position of the column name row, or 0 when absent
     */
    public int nameRow() {
        return nameRow >= 0 ? nameRow : headerRows;
    }

    /**
     * Whether padding is ignored when comparing.
     *
     * @return whether leading and trailing spaces and tabs are ignored
     */
    public boolean trimWhitespace() {
        return trimWhitespace;
    }

    /**
     * Whether a fixed width column ignores its trailing pad.
     *
     * @return whether {@code CHAR(n)} ignores trailing pad even with trimming off
     */
    public boolean charIgnorePad() {
        return charIgnorePad;
    }

    /**
     * Whether numbers compare by value rather than by text.
     *
     * @return whether {@code DECIMAL} and {@code INTEGER} compare by value
     */
    public boolean numeric() {
        return numeric;
    }

    /**
     * Whether booleans compare against the truth sets.
     *
     * @return whether {@code BOOLEAN} compares against the truth sets
     */
    public boolean booleans() {
        return booleans;
    }

    /**
     * The words that count as true.
     *
     * @return the true words, or empty when the engine's default set is used
     */
    public Optional<String> boolTrue() {
        return Optional.ofNullable(boolTrue);
    }

    /**
     * The words that count as false.
     *
     * @return the false words, or empty when the engine's default set is used
     */
    public Optional<String> boolFalse() {
        return Optional.ofNullable(boolFalse);
    }

    /**
     * What happens to a column only the target carries.
     *
     * @return whether a column only the target carries is a finding rather
     *         than an error
     */
    public boolean allowAddedColumns() {
        return allowAddedColumns;
    }

    /**
     * What happens to a column only the source carries.
     *
     * @return whether a column only the source carries is a finding rather
     *         than an error. This one is data loss, so it is the one to leave off
     */
    public boolean allowRemovedColumns() {
        return allowRemovedColumns;
    }

    /**
     * Whether reordered rows are reported as moved.
     *
     * @return whether rows outside the longest increasing subsequence are
     *         reported as moved, per spec 6.2
     */
    public boolean detectMoves() {
        return detectMoves;
    }

    /**
     * Whether the source's row order carries meaning.
     *
     * @return whether the source's row order is meaningful. False for a
     *         {@code SELECT} with no {@code ORDER BY}, which would otherwise
     *         report move noise against a file that does have an order
     */
    public boolean sourceOrdered() {
        return sourceOrdered;
    }

    /**
     * Whether a missing key column is refused.
     *
     * @return whether a comparison without a declared key column is refused
     *         rather than falling back to similarity pairing
     */
    public boolean requireKey() {
        return requireKey;
    }

    /**
     * Where a deleted row appears in the report.
     *
     * @return where a deleted row sits in the report
     */
    public DeletedRowPlacement deletedRowPlacement() {
        return deletedRowPlacement;
    }

    /**
     * How alike two unkeyed rows must be to pair.
     *
     * @return how alike two unkeyed rows must be to pair, or 0 for the
     *         engine's default
     */
    public int similarityPercent() {
        return similarityPercent;
    }

    /**
     * How wide the similarity search casts.
     *
     * @return how many candidates the similarity search considers, or 0 for
     *         the engine's default
     */
    public int similarityCandidates() {
        return similarityCandidates;
    }

    /**
     * Whether the declared schema is checked.
     *
     * @return whether the declared types and required markers are checked,
     *         per spec 13.5
     */
    public boolean validate() {
        return validate;
    }

    /**
     * Whether normalization-equal cells are counted.
     *
     * @return whether cells equal only after normalization are counted, which
     *         is what makes a normalization rule auditable rather than invisible
     */
    public boolean countSuppressed() {
        return countSuppressed;
    }

    /**
     * The input size ceiling.
     *
     * @return the input size ceiling in bytes, or 0 for the engine's default
     */
    public long maxBytes() {
        return maxBytes;
    }

    /**
     * The row count ceiling.
     *
     * @return the row ceiling, or 0 for no limit beyond the byte ceiling
     */
    public int maxRows() {
        return maxRows;
    }

    /**
     * The column count ceiling.
     *
     * @return the column ceiling, or 0 for no limit
     */
    public int maxColumns() {
        return maxColumns;
    }

    /**
     * Size of the direct staging buffer each side is fed through.
     *
     * @return the row feed's staging buffer size in bytes. It bounds one
     *         batch, not the table, so a larger value trades memory for fewer
     *         crossings into the engine
     */
    public int stagingBytes() {
        return stagingBytes;
    }

    /**
     * Builds a {@link DiffOptions}. Every setter returns this builder.
     *
     * @see DiffOptions
     */
    public static final class Builder {
        private byte delimiter = ',';
        private byte quote = '"';
        private boolean stripBom = true;

        private int headerRows = 4;
        private boolean headerRowsStated;
        private int keyRow = -1;
        private int requiredRow = -1;
        private int typeRow = -1;
        private int nameRow = -1;

        private boolean trimWhitespace = true;
        private boolean charIgnorePad = true;
        private boolean numeric = true;
        private boolean booleans = true;
        private String boolTrue;
        private String boolFalse;
        private boolean allowAddedColumns;
        private boolean allowRemovedColumns;

        private boolean detectMoves = true;
        private boolean sourceOrdered = true;
        private boolean requireKey;
        private DeletedRowPlacement deletedRowPlacement = DeletedRowPlacement.ANCHORED;
        private int similarityPercent;
        private int similarityCandidates;

        private boolean validate = true;
        private boolean countSuppressed = true;

        private long maxBytes;
        private int maxRows;
        private int maxColumns;
        private int stagingBytes = 1 << 20;

        private Builder() {
        }

        /**
         * The byte that separates fields. Comma unless set.
         *
         * @param c the delimiter character, which must be one byte in UTF-8
         * @return this builder
         */
        public Builder delimiter(char c) {
            this.delimiter = oneByte(c, "delimiter");
            return this;
        }

        /**
         * The byte that quotes a field. Double quote unless set.
         *
         * @param c the quote character, which must be one byte in UTF-8
         * @return this builder
         */
        public Builder quote(char c) {
            this.quote = oneByte(c, "quote");
            return this;
        }

        /**
         * Drop a leading byte order mark rather than letting it become part of the
         * first column's name.
         *
         * @param v true to strip it
         * @return this builder
         */
        public Builder stripBom(boolean v) {
            this.stripBom = v;
            return this;
        }

        /**
         * Header rows in the <em>source</em> file: 4, or 1 for a names-only file, or
         * 0 for no header at all.
         *
         * <p>The target's count is always auto-detected against the source, per spec
         * 13.8, because the source is authoritative and a sales user may have
         * uploaded a file that kept only the column name row.
         *
         * <p>This is for a <em>byte</em> source, whose header is inside its own
         * bytes. A row source carries a {@link Header} that already states the
         * count, and stating a different one here is rejected rather than silently
         * resolved. See {@link DiffOptions#headerLayout(Header)}.
         *
         * @param rows 4, or 1 for a names-only file, or 0 for no header
         * @return this builder
         */
        public Builder headerRows(int rows) {
            if (rows < 0 || rows > 4) {
                throw new CsvDiffException("headerRows must be 0, 1 or 4, received " + rows);
            }
            this.headerRows = rows;
            this.headerRowsStated = true;
            return this;
        }

        /**
         * 1 based position of the key marker row within the header, 0 for absent.
         *
         * @param row the 1 based row position, or 0 for absent
         * @return this builder
         */
        public Builder keyRow(int row) {
            this.keyRow = row;
            return this;
        }

        /**
         * 1 based position of the required marker row within the header, 0 for
         * absent.
         *
         * @param row the row position
         * @return this builder
         */
        public Builder requiredRow(int row) {
            this.requiredRow = row;
            return this;
        }

        /**
         * 1 based position of the declared type row within the header, 0 for absent.
         *
         * @param row the row position
         * @return this builder
         */
        public Builder typeRow(int row) {
            this.typeRow = row;
            return this;
        }

        /**
         * 1 based position of the column name row within the header, 0 for absent.
         *
         * @param row the row position
         * @return this builder
         */
        public Builder nameRow(int row) {
            this.nameRow = row;
            return this;
        }

        /**
         * Leading and trailing spaces and tabs are ignored when comparing.
         *
         * @param v true to ignore padding
         * @return this builder
         */
        public Builder trimWhitespace(boolean v) {
            this.trimWhitespace = v;
            return this;
        }

        /**
         * {@code CHAR(n)} ignores trailing pad even when trimming is off.
         *
         * @param v true to ignore a fixed width column's trailing pad
         * @return this builder
         */
        public Builder charIgnorePad(boolean v) {
            this.charIgnorePad = v;
            return this;
        }

        /** {@code DECIMAL} and {@code INTEGER} compare by value, so 1.50 equals 1.5
         *  and 007 equals 7. This is what stops an Excel round trip reporting a
         * whole file as changed.
         *
         * @param v true to compare numbers by value
         * @return this builder
         */
        public Builder numeric(boolean v) {
            this.numeric = v;
            return this;
        }

        /**
         * {@code BOOLEAN} compares against the truth sets.
         *
         * @param v true to compare booleans against the truth sets
         * @return this builder
         */
        public Builder booleans(boolean v) {
            this.booleans = v;
            return this;
        }

        /** Comma separated, matched case insensitively. The default is
         *  {@code TRUE,T,YES,Y,1} against {@code FALSE,F,NO,N,0}, and a value in
         * neither set is compared as trimmed bytes rather than coerced.
         *
         * @param trueWords comma separated words counting as true
         * @param falseWords comma separated words counting as false
         * @return this builder
         */
        public Builder booleanWords(String trueWords, String falseWords) {
            this.boolTrue = trueWords;
            this.boolFalse = falseWords;
            return this;
        }

        /**
         * A column in the uploaded file that the source does not declare becomes a
         * finding rather than an error.
         *
         * <p>Never relaxes column <em>order</em>, and a missing key column is always
         * an error. Spec 13.10.
         *
         * @param v true to make it a finding rather than an error
         * @return this builder
         */
        public Builder allowAddedColumns(boolean v) {
            this.allowAddedColumns = v;
            return this;
        }

        /** A column the source declares that the uploaded file does not carry
         *  becomes a finding rather than an error. This one is data loss, so it is
         * the one most callers should leave off.
         *
         * @param v true to make it a finding rather than an error
         * @return this builder
         */
        public Builder allowRemovedColumns(boolean v) {
            this.allowRemovedColumns = v;
            return this;
        }

        /** Off reports no row as moved and skips the longest increasing subsequence
         * entirely.
         *
         * @param v true to report reordered rows as moved
         * @return this builder
         */
        public Builder detectMoves(boolean v) {
            this.detectMoves = v;
            return this;
        }

        /**
         * The caller's assertion that the source side has a meaningful row order. A
         * CSV file always does; an unordered {@code SELECT} does not.
         *
         * <p>When false, move detection is forced off and reported as forced off in
         * the summary rather than silently ignored, and deleted rows go to the end.
         *
         * @param v the new value
         * @return this builder
         */
        public Builder sourceOrdered(boolean v) {
            this.sourceOrdered = v;
            return this;
        }

        /** Fail when the schema declares no key column, instead of taking the
         * all-keys similarity path of spec 6.4.
         *
         * @param v the new value
         * @return this builder
         */
        public Builder requireKey(boolean v) {
            this.requireKey = v;
            return this;
        }

        /**
         * Where a deleted row sits in the report.
         *
         * @param v the placement
         * @return this builder
         */
        public Builder deletedRowPlacement(DeletedRowPlacement v) {
            this.deletedRowPlacement = v;
            return this;
        }

        /** All-keys pairing threshold, as an integer percentage. It is an integer
         *  rather than a fraction so the result cannot depend on floating point,
         * which must be identical between builds.
         *
         * @param percent the new value
         * @return this builder
         */
        public Builder similarityPercent(int percent) {
            this.similarityPercent = percent;
            return this;
        }

        /**
         * Candidates considered per row in all-keys pairing.
         *
         * @param k the new value
         * @return this builder
         */
        public Builder similarityCandidates(int k) {
            this.similarityCandidates = k;
            return this;
        }

        /** The validation findings of spec 13.5. Off gives a pure diff where only
         * the edit script matters.
         *
         * @param v true to check the declared types and required markers
         * @return this builder
         */
        public Builder validate(boolean v) {
            this.validate = v;
            return this;
        }

        /** Count cells that are equal only because normalization suppressed a
         * difference.
         *
         * @param v the new value
         * @return this builder
         */
        public Builder countSuppressed(boolean v) {
            this.countSuppressed = v;
            return this;
        }

        /** Per input file, enforced as bytes arrive. 0 selects the engine's 150 MB
         * default, which sits at the stated real world maximum.
         *
         * @param bytes the new value
         * @return this builder
         */
        public Builder maxBytes(long bytes) {
            this.maxBytes = bytes;
            return this;
        }

        /**
         * Row ceiling. 0 means no limit beyond the byte ceiling.
         *
         * @param rows the ceiling in rows
         * @return this builder
         */
        public Builder maxRows(int rows) {
            this.maxRows = rows;
            return this;
        }

        /**
         * Column ceiling. 0 means no limit.
         *
         * @param columns the ceiling in columns
         * @return this builder
         */
        public Builder maxColumns(int columns) {
            this.maxColumns = columns;
            return this;
        }

        /** Size of the direct staging buffer. Larger means fewer crossings and more
         * resident memory per concurrent diff.
         *
         * @param bytes the new value
         * @return this builder
         */
        public Builder stagingBytes(int bytes) {
            if (bytes < 4096) throw new CsvDiffException("stagingBytes must be at least 4096");
            this.stagingBytes = bytes;
            return this;
        }

        /**
         * The options as set.
         *
         * @return an immutable {@link DiffOptions}
         */
        public DiffOptions build() {
            return new DiffOptions(this);
        }

        private static byte oneByte(char c, String what) {
            if (c > 0x7f) {
                throw new CsvDiffException(
                        what + " must be a single byte, received U+" + Integer.toHexString(c));
            }
            return (byte) c;
        }
    }
}
