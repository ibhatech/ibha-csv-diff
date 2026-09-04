package com.ibhatech.csvdiff;

import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * Where a {@link DiffSource} puts its bytes.
 *
 * <p>The buffer is <strong>direct</strong>, so its address is the address the engine
 * reads from: bytes written here are not copied on their way across the JNI
 * boundary. Fill it, and call {@link #flush()} when it is full or when a batch is
 * complete. One flush is one call into the engine, which is the point: at roughly
 * 1.8 million cells for a 15 MB table, a boundary crossing per field would cost
 * more than the diff.
 *
 * <p><strong>A flush may land anywhere, including inside a field.</strong> The
 * engine's parser is resumable across arbitrary chunk boundaries: a chunk may split
 * a multi byte UTF-8 sequence, a quoted field, a {@code ""} escape pair, a CRLF or
 * the BOM. So a source with a value larger than the buffer writes it in pieces
 * rather than needing a buffer big enough for the largest cell.
 */
public interface ByteFeed {

    /**
     * The staging buffer. Write at its current position; its capacity is the batch
     * size. Never null, and the same buffer is handed back after every flush.
     *
     * @return the staging buffer to write into
     */
    ByteBuffer buffer();

    /**
     * Hands everything written so far to the engine and clears the buffer.
     *
     * @throws IOException if the engine refused the batch
     */
    void flush() throws IOException;

    /**
     * Writes the whole array, flushing as many times as it takes. Callers that
     * already have bytes should use this rather than checking the remaining space
     * themselves.
     *
     * @param bytes the array to write from
     * @param off   where in it to start
     * @param len   how many bytes to write
     * @throws IOException if a flush along the way was refused
     */
    default void write(byte[] bytes, int off, int len) throws IOException {
        int at = off;
        int left = len;
        while (left > 0) {
            ByteBuffer b = buffer();
            if (!b.hasRemaining()) {
                flush();
                b = buffer();
            }
            int n = Math.min(left, b.remaining());
            b.put(bytes, at, n);
            at += n;
            left -= n;
        }
    }

    /**
     * Writes one byte, flushing first if the buffer is full.
     *
     * @param b the byte to write
     * @throws IOException if the flush was refused
     */
    default void write(byte b) throws IOException {
        ByteBuffer buf = buffer();
        if (!buf.hasRemaining()) {
            flush();
            buf = buffer();
        }
        buf.put(b);
    }
}
