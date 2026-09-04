package com.ibhatech.csvdiff.jni;

import com.ibhatech.csvdiff.ByteFeed;
import com.ibhatech.csvdiff.CellDiffMode;
import com.ibhatech.csvdiff.ChangeKind;
import com.ibhatech.csvdiff.CsvDiffException;
import com.ibhatech.csvdiff.Diff;
import com.ibhatech.csvdiff.DiffCell;
import com.ibhatech.csvdiff.DiffOptions;
import com.ibhatech.csvdiff.DiffRow;
import com.ibhatech.csvdiff.DiffSource;
import com.ibhatech.csvdiff.DiffSummary;
import com.ibhatech.csvdiff.EmitFormat;
import com.ibhatech.csvdiff.EmitOptions;
import com.ibhatech.csvdiff.Finding;
import com.ibhatech.csvdiff.FindingKind;
import com.ibhatech.csvdiff.HeaderLayout;
import com.ibhatech.csvdiff.RowOptions;
import com.ibhatech.csvdiff.TextSegment;

import java.io.IOException;
import java.io.OutputStream;
import java.io.UncheckedIOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Optional;
import java.util.OptionalInt;
import java.util.Spliterator;
import java.util.Spliterators;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

/**
 * The JNI backend.
 *
 * <p>One comparison, one context, and the context owns every allocation the engine
 * made for it. {@link #close()} releases the whole arena in one call, which is what
 * makes a batch of comparisons flat rather than monotonically growing.
 *
 * <p>What crosses the boundary, and how often: bytes in, one call per staging
 * buffer; report rows out, one call per batch; cell values, never, because they are
 * read out of the columnar arrays by {@link TableView}. The only per cell call in
 * the binding is {@code ibha_csvd_field_copy} for a field carrying a {@code ""}
 * pair.
 */
final class JniDiff implements Diff {

    private static final int SIDE_SOURCE = 0;
    private static final int SIDE_TARGET = 1;

    private final DiffOptions options;

    private long ctx;
    private long compare;
    private long diff;
    private boolean closed;

    private TableView srcTable;
    private TableView tgtTable;
    private int nColumns;
    private List<String> columnNames = List.of();
    private List<Integer> keyColumns = List.of();
    private int[] colSize = new int[0];
    private int[] colScale = new int[0];

    private DiffSummary summaryCache;

    private JniDiff(DiffOptions options) {
        this.options = options;
    }

    static Diff run(DiffSource source, DiffSource target, DiffOptions options) {
        JniDiff d = new JniDiff(options);
        try {
            d.compare(source, target);
            return d;
        } catch (RuntimeException e) {
            d.close();
            throw e;
        }
    }

    private void compare(DiffSource source, DiffSource target) {
        ctx = NativeEngine.ctxNew(options.maxBytes(), options.maxRows(), options.maxColumns());
        if (ctx == 0) throw new CsvDiffException("the engine could not allocate a context");

        compare = NativeEngine.compareOptsNew(
                bit(options.trimWhitespace()), bit(options.charIgnorePad()), bit(options.numeric()),
                bit(options.booleans()), 0,
                utf8OrNull(options.boolTrue().orElse(null)),
                utf8OrNull(options.boolFalse().orElse(null)),
                bit(options.allowAddedColumns()), bit(options.allowRemovedColumns()));
        if (compare == 0) throw new CsvDiffException("the engine could not allocate its options");

        long[] src = parseSide(source, 0, 0);
        long[] tgt = parseSide(target, src[0], src[1]);

        diff = NativeEngine.diffRun(ctx, compare, src[0], src[1], tgt[0], tgt[1],
                bit(options.detectMoves()), bit(options.sourceOrdered()),
                bit(options.countSuppressed()), bit(options.validate()), bit(options.requireKey()),
                options.deletedRowPlacement().ordinal(), options.similarityCandidates(),
                options.similarityPercent());
        if (diff == 0) throw engineError(null);

        // The projected tables, not the parsed ones. Under the column policy of
        // spec 6.6 column c of a report row is column c of these, and reading the
        // parsed table instead silently reads the wrong column, but only when the
        // caller allowed an added or removed column, which makes it a bug that
        // passes every test written against the default settings.
        srcTable = new TableView(NativeEngine.diffTable(diff, SIDE_SOURCE));
        tgtTable = new TableView(NativeEngine.diffTable(diff, SIDE_TARGET));
        nColumns = NativeEngine.diffColumns(diff);
        readSchema(NativeEngine.diffSchema(diff, SIDE_SOURCE));
    }

    /** @return the table and schema pointers for the side just parsed */
    private long[] parseSide(DiffSource input, long expectTable, long expectSchema) {
        boolean isTarget = expectTable != 0;

        // The target's layout is detected against the source, per spec 13.8. The
        // source's is resolved from the options and from whatever the source itself
        // declares, because a row source writes a header of a length only it knows;
        // taking the count from the options alone is what let a names-only row
        // source eat three data rows without a word.
        HeaderLayout h = isTarget
                ? HeaderLayout.AUTO_DETECT
                : options.headerLayout(input.header().orElse(null));

        long parser = NativeEngine.parseBegin(ctx, compare, options.delimiter(), options.quote(),
                bit(options.stripBom()), h.rows(),
                h.keyRow(), h.requiredRow(), h.typeRow(), h.nameRow(),
                expectTable, expectSchema, input.sizeHint());
        if (parser == 0) throw engineError(null);

        Staging staging = new Staging(parser, options.stagingBytes());
        try {
            input.feed(staging);
            staging.flush();
        } catch (IOException e) {
            throw new CsvDiffException("reading the input failed: " + e.getMessage(), e);
        }

        int st = NativeEngine.parseFinish(parser);
        if (st != 0) throw engineError(st);

        return new long[] {NativeEngine.tableOf(parser), NativeEngine.schemaOf(parser)};
    }

    /**
     * The staging buffer, and one call into the engine per fill.
     *
     * <p>It is direct, so the engine reads the bytes where Java wrote them. The
     * parser copies each chunk into its own arena, which is what makes it safe to
     * clear and refill the same buffer rather than allocating one per batch.
     */
    private final class Staging implements ByteFeed {
        private final long parser;
        private final ByteBuffer buffer;

        Staging(long parser, int capacity) {
            this.parser = parser;
            this.buffer = ByteBuffer.allocateDirect(capacity).order(ByteOrder.nativeOrder());
        }

        @Override
        public ByteBuffer buffer() {
            return buffer;
        }

        @Override
        public void flush() {
            int len = buffer.position();
            if (len == 0) return;
            int st = NativeEngine.parseChunk(parser, buffer, len);
            if (st != 0) throw engineError(st);
            buffer.clear();
        }
    }

    /** Column names and key columns, read once, from the report's own header row
     *  rather than from the file: with a projected table they are not the same
     *  list. */
    private void readSchema(long schema) {
        int[] ints = new int[8];
        NativeEngine.schemaInts(schema, ints);
        int nameRow = ints[5];

        byte[] flags = new byte[Math.max(nColumns, 1)];
        byte[] types = new byte[Math.max(nColumns, 1)];
        colSize = new int[Math.max(nColumns, 1)];
        colScale = new int[Math.max(nColumns, 1)];
        NativeEngine.schemaColumns(schema, nColumns, flags, types, colSize, colScale);

        List<String> names = new ArrayList<>(nColumns);
        List<Integer> keys = new ArrayList<>();
        TableView.Cell cell = new TableView.Cell();
        for (int c = 0; c < nColumns; c++) {
            String name = "";
            if (nameRow >= 0) {
                int f = srcTable.fieldIndex(nameRow, c);
                if (f >= 0) {
                    srcTable.decode(f, 0, cell);
                    name = cell.value;
                }
            }
            names.add(name);
            if ((flags[c] & NativeEngine.COL_KEY) != 0) keys.add(c);
        }
        columnNames = List.copyOf(names);
        keyColumns = List.copyOf(keys);
    }

    /* ------------------------------------------------------------- accessors -- */

    @Override
    public List<String> columns() {
        assertLive();
        return columnNames;
    }

    @Override
    public List<Integer> keyColumns() {
        assertLive();
        return keyColumns;
    }

    @Override
    public long bytesReserved() {
        assertLive();
        return NativeEngine.ctxBytesReserved(ctx);
    }

    /* --------------------------------------------------------------- summary -- */

    @Override
    public DiffSummary summary() {
        assertLive();
        if (summaryCache == null) {
            summaryCache = DiffSummary.parse(emit(EmitFormat.SUMMARY, EmitOptions.defaults()));
        }
        return summaryCache;
    }

    /* ------------------------------------------------------------ the cursor -- */

    @Override
    public Stream<DiffRow> rows() {
        return rows(RowOptions.defaults());
    }

    @Override
    public Stream<DiffRow> rows(RowOptions read) {
        assertLive();
        long cursor = NativeEngine.cursorOpen(diff);
        if (cursor == 0) throw engineError(null);
        RowBatch batch = new RowBatch(cursor, read);
        return StreamSupport.stream(
                Spliterators.spliteratorUnknownSize(batch, Spliterator.ORDERED | Spliterator.NONNULL),
                false);
    }

    /**
     * Drains the report a batch at a time.
     *
     * <p>One call into the engine per {@code batchRows} rows rather than one per
     * row, per spec 13.6. The rows in a batch keep their flags, which the cursor
     * would otherwise reuse under them; cell <em>values</em> are not copied, because
     * the table bytes are immutable for the life of the context and are read
     * straight out of the columnar arrays whenever a cell is decoded.
     *
     * <p>The batch is worth less than it looks. Measured on the p90 pair, running
     * the same walk at one crossing per row instead of one per thousand is inside
     * the noise. The decision that carries the throughput is the one below it, that
     * cells are decoded from the columnar arrays rather than fetched one call at a
     * time, because there are twelve times as many cells as rows here and the ratio
     * grows with the table.
     */
    private final class RowBatch implements java.util.Iterator<DiffRow> {
        private final long cursor;
        private final RowOptions read;
        private final ByteBuffer buffer;
        private final IntBuffer ints;
        private final int stride;
        private final byte[] flags;
        private final TableView.Cell cell = new TableView.Cell();

        private int inBatch;
        private int at;
        private boolean drained;
        private DiffRow next;

        RowBatch(long cursor, RowOptions read) {
            this.cursor = cursor;
            this.read = read;
            this.stride = NativeEngine.rowRecordStride(nColumns);
            this.buffer = ByteBuffer.allocateDirect(stride * read.batchRows())
                    .order(ByteOrder.nativeOrder());
            this.ints = buffer.asIntBuffer();
            this.flags = new byte[Math.max(nColumns, 1)];
        }

        @Override
        public boolean hasNext() {
            while (next == null) {
                if (at >= inBatch) {
                    if (drained || !refill()) return false;
                }
                DiffRow row = readRow(at++);
                if (read.changesOnly() && row.isQuiet()) continue;
                next = row;
            }
            return true;
        }

        @Override
        public DiffRow next() {
            if (!hasNext()) throw new java.util.NoSuchElementException();
            DiffRow row = next;
            next = null;
            return row;
        }

        private boolean refill() {
            assertLive();
            int n = NativeEngine.cursorNextBatch(cursor, buffer, read.batchRows(), nColumns);
            if (n < 0) throw engineError(n);
            inBatch = n;
            at = 0;
            if (n < read.batchRows()) drained = true;
            return n > 0;
        }

        private DiffRow readRow(int index) {
            int base = index * stride / 4;
            int kind = ints.get(base + NativeEngine.REC_KIND);
            boolean moved = ints.get(base + NativeEngine.REC_MOVED) != 0;
            int moveDistance = ints.get(base + NativeEngine.REC_MOVE_DISTANCE);
            int sourceRow = ints.get(base + NativeEngine.REC_SOURCE_ROW);
            int targetRow = ints.get(base + NativeEngine.REC_TARGET_ROW);
            int changed = ints.get(base + NativeEngine.REC_CHANGED);
            int suppressed = ints.get(base + NativeEngine.REC_SUPPRESSED);
            int findingCells = ints.get(base + NativeEngine.REC_FINDINGS);

            buffer.get(index * stride + NativeEngine.REC_HEAD_BYTES, flags, 0, nColumns);

            List<DiffCell> cells = read.includeValues()
                    ? decodeCells(sourceRow, targetRow, read.maxCellBytes())
                    : List.of();
            List<Finding> findings = findingCells > 0 ? decodeFindings() : List.of();
            List<String> key = cells.isEmpty() || keyColumns.isEmpty() ? List.of() : keyOf(cells);

            // Record numbers, 1 based, counting parsed records rather than physical
            // lines. Every row number in every emitter uses the same convention.
            return new DiffRow(ChangeKind.ofOrdinal(kind), moved, moveDistance,
                    sourceRow == NativeEngine.NO_ROW ? OptionalInt.empty()
                            : OptionalInt.of(sourceRow + 1),
                    targetRow == NativeEngine.NO_ROW ? OptionalInt.empty()
                            : OptionalInt.of(targetRow + 1),
                    changed, suppressed, cells, findings, key);
        }

        private List<DiffCell> decodeCells(int sourceRow, int targetRow, int maxCellBytes) {
            List<DiffCell> cells = new ArrayList<>(nColumns);
            for (int c = 0; c < nColumns; c++) {
                int fs = srcTable.fieldIndex(sourceRow, c);
                int ft = tgtTable.fieldIndex(targetRow, c);
                int fl = flags[c] & 0xff;

                boolean changed = (fl & NativeEngine.CELL_CHANGED) != 0;
                boolean suppressed = (fl & NativeEngine.CELL_SUPPRESSED) != 0;
                boolean truncated = false;
                boolean invalid = false;

                // The contract, and the one rule a consumer gets wrong: a matched
                // row carries source exactly when the cell differs in bytes from the
                // target. Its absence means the two sides are byte identical, not
                // that the value was empty.
                Optional<String> source = Optional.empty();
                if (fs >= 0 && (ft < 0 || changed || suppressed)) {
                    srcTable.decode(fs, maxCellBytes, cell);
                    source = Optional.of(cell.value);
                    truncated = cell.truncated;
                    invalid = cell.invalidUtf8;
                }
                Optional<String> target = Optional.empty();
                if (ft >= 0) {
                    tgtTable.decode(ft, maxCellBytes, cell);
                    target = Optional.of(cell.value);
                    truncated |= cell.truncated;
                    invalid |= cell.invalidUtf8;
                }

                cells.add(new DiffCell(c, columnNames.get(c), changed, suppressed, source, target,
                        truncated, invalid));
            }
            return cells;
        }

        private List<Finding> decodeFindings() {
            List<Finding> out = new ArrayList<>();
            for (int c = 0; c < nColumns; c++) {
                int fl = flags[c] & 0xff;
                if ((fl & NativeEngine.CELL_FINDING) == 0) continue;
                String name = columnNames.get(c);
                for (FindingKind kind : FindingKind.values()) {
                    if ((fl & kind.bit()) == 0) continue;
                    out.add(switch (kind) {
                        case TOO_LONG -> Finding.tooLong(c, name, colSize[c]);
                        case PRECISION -> Finding.precision(c, name, colSize[c], colScale[c]);
                        default -> Finding.of(c, name, kind);
                    });
                }
            }
            return out;
        }

        private List<String> keyOf(List<DiffCell> cells) {
            List<String> key = new ArrayList<>(keyColumns.size());
            for (int c : keyColumns) {
                DiffCell cc = cells.get(c);
                key.add(cc.target().or(cc::source).orElse(""));
            }
            return key;
        }
    }

    /* -------------------------------------------------------------- segments -- */

    @Override
    public List<TextSegment> cellSegments(DiffRow row, int column, CellDiffMode mode) {
        assertLive();
        if (mode == CellDiffMode.NONE || row.sourceRow().isEmpty() || row.targetRow().isEmpty()) {
            return List.of();
        }
        int src = row.sourceRow().getAsInt() - 1;
        int tgt = row.targetRow().getAsInt() - 1;

        // The count does not depend on the cap, so one sizing call then one filling
        // call is exact.
        int n = NativeEngine.cellSegments(diff, src, tgt, column, mode.ordinal(), 0, null, 0);
        if (n < 0) throw engineError(n);
        if (n == 0) return List.of();

        ByteBuffer out = ByteBuffer.allocateDirect(n * 12).order(ByteOrder.nativeOrder());
        int got = NativeEngine.cellSegments(diff, src, tgt, column, mode.ordinal(), 0, out, n);
        if (got < 0) throw engineError(got);

        // Three uint32 and no padding, which is why this is a plain int read rather
        // than a struct walk.
        IntBuffer ints = out.asIntBuffer();
        List<TextSegment> segments = new ArrayList<>(got);
        for (int i = 0; i < got; i++) {
            segments.add(new TextSegment(TextSegment.Op.ofOrdinal(ints.get(i * 3)),
                    ints.get(i * 3 + 1), ints.get(i * 3 + 2)));
        }
        return Collections.unmodifiableList(segments);
    }

    /* ------------------------------------------------------------------ emit -- */

    @Override
    public byte[] emit(EmitFormat format) {
        return emit(format, EmitOptions.defaults());
    }

    @Override
    public byte[] emit(EmitFormat format, EmitOptions opts) {
        assertLive();
        byte[] prefix = utf8OrNull(opts.classPrefix().orElse(null));

        // Sized in two passes: one with no buffer at all, which allocates nothing
        // and reports exactly how many bytes the report is, then one that fills it.
        // The buffer sink counts past the end rather than truncating, which is what
        // makes the first pass a measurement.
        long size = emitInto(format, opts, prefix, null, 0);
        if (size == 0) return new byte[0];
        if (size > Integer.MAX_VALUE) {
            throw new CsvDiffException("the report is " + size
                    + " bytes, which does not fit a Java array. Use changesOnly or maxRows");
        }

        ByteBuffer out = ByteBuffer.allocateDirect((int) size);
        long written = emitInto(format, opts, prefix, out, (int) size);
        if (written != size) {
            throw new CsvDiffException(
                    "the emitter produced " + written + " bytes on the second pass and " + size
                            + " on the first");
        }
        byte[] bytes = new byte[(int) written];
        out.get(0, bytes);
        return bytes;
    }

    @Override
    public long emitTo(OutputStream out, EmitFormat format, EmitOptions opts) {
        byte[] bytes = emit(format, opts);
        try {
            out.write(bytes);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
        return bytes.length;
    }

    private long emitInto(EmitFormat format, EmitOptions o, byte[] prefix, ByteBuffer out, int cap) {
        long r = NativeEngine.emit(diff, format.ordinal(), bit(o.changesOnly()),
                bit(o.includeValues()), o.cellDiff().ordinal(), o.maxCellBytes(), o.maxRows(),
                bit(o.csvFormulaGuard()), o.csvDelimiter(), prefix, out, cap);
        if (r < 0) throw engineError((int) r);
        return r;
    }

    /* ------------------------------------------------------------ lifecycle -- */

    @Override
    public void close() {
        if (closed) return;
        closed = true;
        if (compare != 0) {
            NativeEngine.compareOptsFree(compare);
            compare = 0;
        }
        if (ctx != 0) {
            NativeEngine.ctxFree(ctx);
            ctx = 0;
        }
        diff = 0;
    }

    private void assertLive() {
        if (closed) throw new CsvDiffException("this diff has been closed");
    }

    /* --------------------------------------------------------------- errors -- */

    private CsvDiffException engineError(Integer status) {
        int st = status != null ? status : NativeEngine.ctxStatus(ctx);
        String name = NativeEngine.statusName(st);
        byte[] detail = NativeEngine.ctxError(ctx);
        String message = detail == null || detail.length == 0
                ? name
                : new String(detail, StandardCharsets.UTF_8);
        return new CsvDiffException(message, name, null);
    }

    private static int bit(boolean v) {
        return v ? 1 : 0;
    }

    private static byte[] utf8OrNull(String s) {
        return s == null ? null : s.getBytes(StandardCharsets.UTF_8);
    }
}
