package com.ibhatech.csvdiff.jni;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import java.nio.charset.StandardCharsets;

/**
 * A reader over one {@code ibha_csvd_table *}.
 *
 * <p><strong>Cells are decoded out of the columnar arrays, never through a call per
 * cell.</strong> The public header says so about this binding by name, and the
 * arithmetic is why: the p90 pair is 1.8 million cells, so at even 30 ns a
 * crossing, asking the engine for each cell would cost about 50 ms per pass more
 * than the entire match. So the four arrays arrive once as direct buffers over
 * engine memory and everything below is plain buffer reads.
 *
 * <p>The fast path never calls the engine at all: {@code field_off} points past the
 * opening quote and {@code field_len} excludes the closing one, so for a field with
 * no {@code ""} pair that byte range <em>is</em> the logical value. Only an escaped
 * field needs {@code ibha_csvd_field_copy} to collapse the pairs, and the engine
 * counts those separately precisely because they are rare.
 *
 * <p>Unlike the JS binding, there is nothing here about buffers being detached and
 * re-derived. Linear memory in wasm is replaced by {@code memory.grow}, which
 * invalidates every view over it; a native arena hands out addresses that stay put
 * for the life of the context. The lifetime that does matter is the context's, and
 * {@link JniDiff} is what refuses to decode after it has been closed.
 */
final class TableView {

    private final long table;
    private final ByteBuffer bytes;
    private final IntBuffer fieldOff;
    private final IntBuffer fieldLen;
    private final ByteBuffer fieldFlags;
    private final IntBuffer rowFirstField;

    private final int nRows;
    private final int nColumns;
    private final int nFields;

    /** Reused across cells, so decoding a report costs one String per cell and not
     *  also one array. Grown on demand and never shrunk. */
    private byte[] scratch = new byte[256];

    /** For the escaped path only: the engine writes the collapsed value here. */
    private ByteBuffer escaped;

    TableView(long table) {
        this.table = table;
        int[] dims = new int[5];
        NativeEngine.tableDims(table, dims);
        this.nRows = dims[0];
        this.nColumns = dims[1];
        this.nFields = dims[2];

        this.bytes = order(NativeEngine.tableBytes(table));
        this.fieldOff = intsOf(NativeEngine.tableFieldOff(table));
        this.fieldLen = intsOf(NativeEngine.tableFieldLen(table));
        this.fieldFlags = order(NativeEngine.tableFieldFlags(table));
        this.rowFirstField = intsOf(NativeEngine.tableRowFirstField(table));
    }

    private static ByteBuffer order(ByteBuffer b) {
        return b == null ? ByteBuffer.allocateDirect(0) : b.order(ByteOrder.nativeOrder());
    }

    private static IntBuffer intsOf(ByteBuffer b) {
        return order(b).asIntBuffer();
    }

    int rowCount() {
        return nRows;
    }

    int columnCount() {
        return nColumns;
    }

    /**
     * The field index of a cell, or -1 when the row or column is out of range.
     *
     * <p>This is {@code ibha_csvd_row_field} restated over the same array it reads,
     * including its bounds check against the next row's first field, which is what
     * makes a ragged row report as a short row rather than reading into the next
     * one. Restated rather than called because it is the one operation that happens
     * per cell.
     */
    int fieldIndex(int row, int col) {
        if (row < 0 || row >= nRows || col < 0) return -1;
        int first = rowFirstField.get(row);
        if (col >= rowFirstField.get(row + 1) - first) return -1;
        return first + col;
    }

    boolean isEscaped(int field) {
        return (fieldFlags.get(field) & NativeEngine.FIELD_HAS_ESCAPE) != 0;
    }

    /**
     * Decodes a cell's logical value into {@code out}.
     *
     * <p>{@code maxBytes} cuts at a UTF-8 boundary rather than mid sequence, so a
     * truncated value is still decodable text.
     */
    void decode(int field, int maxBytes, Cell out) {
        out.truncated = false;
        out.invalidUtf8 = false;

        int len;
        if (!isEscaped(field)) {
            int off = fieldOff.get(field);
            len = fieldLen.get(field);
            ensure(len);
            copyInto(bytes, off, len);
        } else {
            len = copyEscaped(field);
        }

        if (maxBytes > 0 && len > maxBytes) {
            int cut = maxBytes;
            // 0b10xxxxxx is a continuation byte; back off until the cut lands on
            // the start of a sequence.
            while (cut > 0 && (scratch[cut] & 0xc0) == 0x80) cut--;
            len = cut;
            out.truncated = true;
        }

        // Checking validity before decoding, rather than looking for U+FFFD in the
        // result afterwards, because that cannot tell a replacement from a
        // replacement character the file genuinely contained. The scan is one pass
        // over bytes already in cache and costs nothing on the ASCII path.
        out.invalidUtf8 = !Utf8.isValid(scratch, len);
        out.value = new String(scratch, 0, len, StandardCharsets.UTF_8);
    }

    private void copyInto(ByteBuffer src, int off, int len) {
        src.get(off, scratch, 0, len);
    }

    private int copyEscaped(int field) {
        int need = NativeEngine.fieldLogicalLen(table, field);
        if (escaped == null || escaped.capacity() < need) {
            escaped = ByteBuffer.allocateDirect(Math.max(need, 1 << 16));
        }
        int n = NativeEngine.fieldCopy(table, field, escaped, escaped.capacity());
        if (n < 0) throw new IllegalStateException("the engine refused to copy field " + field);
        ensure(n);
        copyInto(escaped, 0, n);
        return n;
    }

    private void ensure(int n) {
        if (scratch.length < n) {
            int size = scratch.length;
            while (size < n) size <<= 1;
            scratch = new byte[size];
        }
    }

    int fieldCount() {
        return nFields;
    }

    /** One decoded cell, reused by the caller so a walk allocates one String per
     *  cell rather than one String and one wrapper. */
    static final class Cell {
        String value = "";
        boolean truncated;
        boolean invalidUtf8;
    }
}
