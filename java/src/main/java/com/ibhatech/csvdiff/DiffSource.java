package com.ibhatech.csvdiff;

import java.io.IOException;
import java.io.InputStream;
import java.io.Reader;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.sql.Clob;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.Iterator;
import java.util.List;
import java.util.Optional;

/**
 * One side of a comparison: where its bytes come from.
 *
 * <p>Bytes, never strings. A Java {@code String} of a 15 MB file is 30 MB of char
 * data plus an encode pass to get back to the UTF-8 the engine reads, and the
 * things a caller actually has, a file, an {@code InputStream}, a result set, are
 * all already bytes or can produce them without going through one.
 *
 * <p>The interface is public so a caller with a source this class does not name, an
 * S3 object, an FTP socket, a proprietary reader, can implement it in a dozen lines
 * and get the same streaming behaviour as everything here.
 *
 * <h2>Character data</h2>
 *
 * <p>A CLOB or an {@code NVARCHAR} column is characters, not bytes, so
 * {@link #ofClob(Clob)}, {@link #ofReader(Reader)} and {@link #ofString(CharSequence)}
 * encode as they go, a chunk at a time, straight into the staging buffer. They exist
 * so that the two things a caller would otherwise reach for stop being the only
 * options: {@code getAsciiStream}, which corrupts every non-ASCII byte, and
 * {@code getString(n).getBytes(UTF_8)}, which is correct and holds the text three
 * times at peak.
 *
 * <h2>Result sets</h2>
 *
 * <p>{@link #ofResultSet(ResultSet, Header)} takes the header <em>separately</em>,
 * because a result set is data rows only and nothing in it says which columns are
 * keys. See {@link Header}.
 *
 * <h2>A header the data does not carry</h2>
 *
 * <p>{@link #withHeader(Header, DiffSource)} writes header rows ahead of another
 * source's bytes, for the common case of a column holding headerless data, or a name
 * row only, while the keys and types live in a metadata table.
 */
public interface DiffSource {

    /**
     * Streams every byte of this source into the feed. Fill {@code out.buffer()} and
     * call {@code out.flush()}; each flush is one call into the engine.
     *
     * @param out where to write, and the thing to flush
     * @throws IOException if reading this source, or handing a batch to the engine,
     *                     failed
     */
    void feed(ByteFeed out) throws IOException;

    /**
     * The header this source writes ahead of its first data row, when it writes one
     * at all.
     *
     * <p>Empty for a byte source: a file, a stream or a byte array carries its own
     * header inside its own bytes and nothing out here knows how many rows of it
     * there are, which is what {@link DiffOptions.Builder#headerRows(int)} is for.
     *
     * <p>A row source is the other case. It writes exactly
     * {@link Header#rowCount()} rows, it knows that number, and the parse has to be
     * told the same number or it reads data rows as header rows. Returning the
     * header here is what lets {@link DiffOptions#headerLayout(Header)} reconcile
     * the two instead of leaving them to agree by luck.
     *
     * @return the header this source writes ahead of its data, or empty when
     *         its header is inside its own bytes
     */
    default Optional<Header> header() {
        return Optional.empty();
    }

    /**
     * The expected total byte count, or 0 when it is not known.
     *
     * <p>Worth supplying whenever it is cheap. The engine sizes its byte buffer and
     * its index arrays from it; without it they double as they grow and the arena
     * does not reclaim the abandoned copies. On the p90 pair that is 181 MB reserved
     * against 86 MB, a factor of 2.1, which is what sets how many concurrent diffs
     * fit on a batch worker. {@code StreamSourceTest} is where that is measured; do
     * not restate it from memory here.
     *
     * @return the byte count if known, else 0. A hint sizes the engine's
     *         reservation once instead of growing it
     */
    default long sizeHint() {
        return 0;
    }

    /**
     * Bytes the caller already holds.
     *
     * @param bytes the CSV bytes
     * @return a source over those bytes
     */
    static DiffSource ofBytes(byte[] bytes) {
        return new BytesSource(bytes);
    }

    /**
     * A file, read straight into the engine's staging buffer.
     *
     * <p>The read is into a direct buffer, so the bytes go from the operating system
     * into native memory without passing through the Java heap at all.
     *
     * @param path the file to read
     * @return a source over that file
     */
    static DiffSource ofFile(Path path) {
        return new FileSource(path);
    }

    /**
     * Any stream, of unknown length. Closed by the caller, not by this.
     *
     * <p>Prefer {@link #ofStream(InputStream, long)} whenever the length is
     * knowable, which for the streams that actually turn up here it usually is.
     *
     * @param in the stream to read, which this does not close
     * @return a source over that stream
     */
    static DiffSource ofStream(InputStream in) {
        return new StreamSource(in, 0);
    }

    /**
     * A stream whose length the caller already knows.
     *
     * <p>Worth the one extra argument. Without a hint the engine's byte buffer and
     * index arrays double as they grow and the arena does not reclaim the abandoned
     * copies. Measured on the p90 pair through {@link Diff#bytesReserved()}, the
     * whole comparison reserves <strong>86 MB hinted against 181 MB unhinted, a
     * factor of 2.1</strong>, and that is what sets how many concurrent diffs fit on
     * a batch worker. The streams a server actually feeds this nearly all know their
     * size: a multipart part, an HTTP {@code Content-Length}, an S3 object's
     * {@code ContentLength}, a {@code Blob.length()}. Passing it gives a stream fed
     * side the reservation profile of {@link #ofFile(Path)}, which gets its own hint
     * from {@code Files.size}.
     *
     * <p><strong>It is a hint and never a limit.</strong> It does not truncate the
     * read, it does not extend it, and it is not checked against what arrives, so a
     * stale or approximate value costs a little performance and never correctness.
     * The ceiling on what a side may be is {@code maxBytes} in {@link DiffOptions},
     * which is a different thing and is enforced.
     *
     * @param in   the stream, closed by the caller
     * @param size the expected total byte count, or 0 when it is not known after all
     *
     * @return a source over that stream
     */
    static DiffSource ofStream(InputStream in, long size) {
        if (size < 0) {
            throw new CsvDiffException("a size hint cannot be negative, received " + size);
        }
        return new StreamSource(in, size);
    }

    /**
     * Text the caller already holds: a {@code String} from
     * {@code rs.getString(n)}, a {@code StringBuilder}, any {@link CharSequence}.
     *
     * <p>Encoded straight into the staging buffer through a wrapping view, so the
     * text is not copied and no intermediate {@code byte[]} of it ever exists. That
     * is the difference from {@code ofBytes(text.getBytes(UTF_8))}, which on a 15 MB
     * column allocates a 15 MB array that lives until the diff is done.
     *
     * @param text the CSV text
     * @return a source over that text, encoded UTF-8 as it is fed
     */
    static DiffSource ofString(CharSequence text) {
        if (text == null) throw new CsvDiffException("a text source needs text, received null");
        return new StringSource(text);
    }

    /**
     * Any character stream, encoded to UTF-8 as it is read. Closed by the caller,
     * not by this.
     *
     * <p>The encode is chunked: a fixed block of chars at a time, written directly
     * into the direct staging buffer, including across the chunk boundary that falls
     * between the two halves of a surrogate pair. A {@code Reader} source that read
     * the whole thing into a {@code String} first would reintroduce exactly the heap
     * cost these factories exist to remove.
     *
     * <p>Prefer {@link #ofReader(Reader, long)} when the byte count is knowable.
     *
     * @param in the stream to read, which this does not close
     * @return a source over that reader, encoded UTF-8 as it is fed
     */
    static DiffSource ofReader(Reader in) {
        return new ReaderSource(in, 0);
    }

    /**
     * A character stream whose encoded size the caller already knows.
     *
     * <p>Worth the extra argument for the same reason as
     * {@link #ofStream(InputStream, long)}: it is what decides whether the engine
     * reserves once or doubles its arrays as it grows.
     *
     * <p><strong>The hint is bytes, not characters</strong>, because bytes are what
     * gets reserved. A caller who knows only the character count should pass it
     * anyway: for ASCII the two are equal, for anything else it undercounts, and
     * undercounting a hint costs a little growth and never correctness.
     *
     * @param in   the reader, closed by the caller
     * @param size the expected encoded byte count, or 0 when it is not known
     *
     * @return a source over that reader, encoded UTF-8 as it is fed
     */
    static DiffSource ofReader(Reader in, long size) {
        if (size < 0) {
            throw new CsvDiffException("a size hint cannot be negative, received " + size);
        }
        return new ReaderSource(in, size);
    }

    /**
     * A CLOB, read as characters and encoded to UTF-8 as it streams.
     *
     * <p>This is use case 2 and 3 of {@code specs/input-options.md}: the original
     * side of a comparison living in a CLOB or {@code NVARCHAR(max)} column. Read
     * through {@link Clob#getCharacterStream()}, which is the only accessor that
     * preserves the text, and never through {@code getAsciiStream}, which replaces
     * every non-ASCII character with a question mark and reports a clean comparison
     * of corrupted data.
     *
     * <p>The character stream is opened by this and closed by this. The
     * {@code Clob} itself is the caller's and stays open, so a caller reading both
     * sides out of one row is unaffected.
     *
     * <p>A CLOB holding <em>headerless</em> rows, or a name row only while the keys
     * and types live elsewhere, is the usual shape. Wrap it:
     * {@code DiffSource.withHeader(header, DiffSource.ofClob(clob))}.
     *
     * @param clob the character column to read
     * @return a source over that column, which this opens and closes
     */
    static DiffSource ofClob(Clob clob) {
        if (clob == null) throw new CsvDiffException("a clob source needs a clob, received null");
        return new ClobSource(clob);
    }

    /**
     * Writes {@code header}'s rows ahead of another source's bytes, so that data
     * carrying no header of its own reaches the engine as one ordinary CSV.
     *
     * <p>The case it is for: a CLOB or a file holding data rows only, or holding a
     * name row while which columns are keys and what their types are lives in a
     * metadata table or a schema registry. Nothing about that data can say it, and
     * the header is not something this library can derive, so the caller states it
     * once here rather than concatenating strings by hand.
     *
     * {@snippet :
     * DiffSource original = DiffSource.withHeader(header, DiffSource.ofClob(clob));
     * DiffSource uploaded = DiffSource.ofBytes(upload);       // carries its own
     * }
     *
     * <p><strong>The header row count follows automatically.</strong> This reports
     * {@code header} from {@link #header()}, so the parse is told the same number of
     * header rows that were written, rather than the two agreeing by luck. Stating a
     * different {@link DiffOptions.Builder#headerRows(int)} is rejected rather than
     * silently resolved. See {@link DiffOptions#headerLayout(Header)}.
     *
     * <p><strong>Two things this does not do.</strong> It does not check that the
     * wrapped bytes have {@code header.columnCount()} columns, because they are not
     * parsed here; a mismatch surfaces as the engine's own ragged row error. And a
     * byte order mark inside the wrapped source is no longer at offset 0 once the
     * header sits in front of it, so it stops being a BOM and becomes part of the
     * first field: strip it before wrapping, or read that side as characters through
     * {@link #ofReader(Reader)}, where it is a character rather than three bytes.
     *
     * @param header the header rows to write, and the count the parse will use
     * @param body   the data rows, which must not declare a header of their own
     *
     * @return a source writing the header rows and then the body
     */
    static DiffSource withHeader(Header header, DiffSource body) {
        return new HeaderedSource(header, body, RowFeedOptions.defaults());
    }

    /** As {@link #withHeader(Header, DiffSource)}, with the dialect the header rows
     *  are written in under the caller's control. It has to match what the engine is
     *  told to parse, which for anything but the default delimiter means setting
     * both.
     *
     * @param header the header describing the columns, including keys and types
     * @param body the source whose bytes follow, which must not itself
     *             declare a header
     * @param options the row feed settings
     * @return a source writing the header rows and then the body
     */
    static DiffSource withHeader(Header header, DiffSource body, RowFeedOptions options) {
        return new HeaderedSource(header, body, options);
    }

    /**
     * A JDBC result set, with the header rows supplied alongside it.
     *
     * <p>The rows are encoded as CSV into the direct staging buffer and handed to
     * the engine a batch at a time, per spec 13.6, so a result set and a file reach
     * the same parser and cannot be compared under different rules. Values are
     * rendered by the explicit per-type rules documented on this package rather than
     * by {@code rs.getString}, which differs between drivers.
     *
     * <p>The result set is read forward once and is not closed by this.
     *
     * @param rs     positioned before the first row
     * @param header the header rows. A result set carries none, and which columns
     *               are keys is not something JDBC can tell you
     *
     * @return a source over those rows
     */
    static DiffSource ofResultSet(ResultSet rs, Header header) {
        return new ResultSetSource(rs, header, RowFeedOptions.defaults());
    }

    /** As {@link #ofResultSet(ResultSet, Header)}, with the batch size and dialect
     * under the caller's control.
     *
     * @param rs the result set to read, which this does not close
     * @param header the header describing the columns, including keys and types
     * @param options the row feed settings
     * @return a source over those rows
     */
    static DiffSource ofResultSet(ResultSet rs, Header header, RowFeedOptions options) {
        return new ResultSetSource(rs, header, options);
    }

    /**
     * Any row producer at all: an iterator of already rendered values.
     *
     * <p>This is the row feed without JDBC. A caller reading from a message queue, a
     * Parquet file or an in memory list gets the same path, and the JDBC source
     * above is a thin adapter over exactly this.
     *
     * @param header the header describing the columns, including keys and types
     * @param rows the rows, pulled one at a time and never all held at once
     * @return a source over those rows
     */
    static DiffSource ofRows(Header header, Iterator<? extends List<String>> rows) {
        return new RowsSource(header, rows, RowFeedOptions.defaults());
    }

    /**
     * As {@link #ofRows(Header, Iterator)}, with the batch size and dialect under
     * the caller's control.
     *
     * @param header  the header describing the columns, including keys and types
     * @param rows    the rows, pulled one at a time and never all held at once
     * @param options the row feed settings
     * @return a source over those rows
     */
    static DiffSource ofRows(Header header, Iterator<? extends List<String>> rows,
                             RowFeedOptions options) {
        return new RowsSource(header, rows, options);
    }

    /**
     * A JSON array of objects, read one row at a time.
     *
     * <p>Use case 3 of {@code specs/input-options.md}: a table held in a column as
     * {@code [{"id":"1","name":"a"}, ...]} rather than as CSV text. It feeds the same
     * row path as {@link #ofRows(Header, Iterator)}, so it inherits the batch feed,
     * the escaping and the width check, and reaches the same parser as a file.
     *
     * <p>The reader is closed by the caller. Prefer {@link #ofJsonRows(Header, Clob)}
     * when the JSON is in a CLOB, which opens and closes its own stream.
     *
     * <h4>What it accepts, and what it refuses</h4>
     *
     * <p><strong>An array of objects, and only that.</strong> The array of arrays
     * shape was deliberately not built rather than built speculatively.
     *
     * <p>The {@code header} decides the column order, because JSON objects are
     * unordered and two rows of one export can list their keys differently. Each
     * object is projected by name.
     *
     * <p>Errors, all of them naming the row and the position: a key the header
     * declares that the object does not carry, a key the header does not declare, the
     * same key twice, and a nested object or array as a value. The first would
     * otherwise report as a changed value against a file that has the real one; the
     * second is the JSON form of an added column; the third is undefined in JSON
     * itself; the fourth has no text form a CSV cell would agree with.
     *
     * <p>Values render by exactly the rules {@link #ofResultSet(ResultSet, Header)}
     * uses, through the same code, because use case 3 compares a JSON side against a
     * JDBC side of the same data. So JSON {@code null} is an empty field and
     * {@code true} is {@code TRUE}. <strong>A number keeps its literal text</strong>
     * and never passes through a {@code double}: {@code 1.50} has to survive as
     * {@code 1.50} for a {@code DECIMAL(12,2)} comparison to mean anything, and a
     * value wider than a {@code double} has to survive at all.
     *
     * @param header the column names to project each object into, and the header rows
     *               written ahead of the data. A JSON array carries neither
     * @param json   the array, read once, closed by the caller
     *
     * @return a source over that JSON array
     */
    static DiffSource ofJsonRows(Header header, Reader json) {
        return new JsonRowsSource(header, json, null, RowFeedOptions.defaults());
    }

    /** As {@link #ofJsonRows(Header, Reader)}, with the batch size and dialect under
     *
     * @param header the header describing the columns, including keys and types
     * @param json the JSON array of objects
     * @param options the row feed settings
     * @return a source over that JSON array
     */
    static DiffSource ofJsonRows(Header header, Reader json, RowFeedOptions options) {
        return new JsonRowsSource(header, json, null, options);
    }

    /**
     * As {@link #ofJsonRows(Header, Reader)}, from a byte stream. Decoded as UTF-8,
     * which RFC 8259 requires of JSON exchanged between systems.
     *
     * @param header the header describing the columns, including keys and types
     * @param json the JSON array of objects
     * @return a source over that JSON array
     */
    static DiffSource ofJsonRows(Header header, InputStream json) {
        if (json == null) throw new CsvDiffException("a JSON source needs a stream, received null");
        return new JsonRowsSource(header,
                new java.io.InputStreamReader(json, java.nio.charset.StandardCharsets.UTF_8),
                null, RowFeedOptions.defaults());
    }

    /**
     * As {@link #ofJsonRows(Header, Reader)}, straight from a CLOB, which is where
     * use case 3 says the array actually lives.
     *
     * <p>The character stream is opened by this and closed by this, and the
     * {@code Clob} itself is left alone.
     *
     * @param header the header describing the columns, including keys and types
     * @param json the JSON array of objects
     * @return a source over that JSON array
     */
    static DiffSource ofJsonRows(Header header, Clob json) {
        if (json == null) throw new CsvDiffException("a JSON source needs a clob, received null");
        return new JsonRowsSource(header, null, json, RowFeedOptions.defaults());
    }

    /* ------------------------------------------------------ implementations -- */

    /**
     * The {@link #ofBytes(byte[])} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class BytesSource implements DiffSource {
        private final byte[] bytes;

        BytesSource(byte[] bytes) {
            this.bytes = bytes;
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            out.write(bytes, 0, bytes.length);
            out.flush();
        }

        @Override
        public long sizeHint() {
            return bytes.length;
        }
    }

    /**
     * The {@link #ofFile(java.nio.file.Path)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class FileSource implements DiffSource {
        private final Path path;

        FileSource(Path path) {
            this.path = path;
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            try (FileChannel ch = FileChannel.open(path, StandardOpenOption.READ)) {
                ByteBuffer b = out.buffer();
                while (ch.read(b) >= 0) {
                    if (!b.hasRemaining()) {
                        out.flush();
                        b = out.buffer();
                    }
                }
                out.flush();
            }
        }

        @Override
        public long sizeHint() {
            try {
                return Files.size(path);
            } catch (IOException e) {
                return 0;
            }
        }
    }

    /**
     * The {@link #ofStream(java.io.InputStream)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class StreamSource implements DiffSource {
        private final InputStream in;
        private final long size;

        StreamSource(InputStream in, long size) {
            this.in = in;
            this.size = size;
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            byte[] chunk = new byte[1 << 16];
            for (int n = in.read(chunk); n >= 0; n = in.read(chunk)) {
                out.write(chunk, 0, n);
            }
            out.flush();
        }

        @Override
        public long sizeHint() {
            return size;
        }
    }

    /**
     * The {@link #ofString(CharSequence)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class StringSource implements DiffSource {
        private final CharSequence text;

        StringSource(CharSequence text) {
            this.text = text;
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            new Utf8Encoder(out).encode(text);
            out.flush();
        }

        /** Exact, and cheap: one pass over the chars with nothing allocated. */
        @Override
        public long sizeHint() {
            return Utf8Encoder.utf8Length(text);
        }
    }

    /**
     * The {@link #ofReader(java.io.Reader)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class ReaderSource implements DiffSource {
        private final Reader in;
        private final long size;

        ReaderSource(Reader in, long size) {
            this.in = in;
            this.size = size;
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            new Utf8Encoder(out).encode(in);
            out.flush();
        }

        @Override
        public long sizeHint() {
            return size;
        }
    }

    /**
     * The {@link #ofClob(java.sql.Clob)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class ClobSource implements DiffSource {
        private final Clob clob;

        ClobSource(Clob clob) {
            this.clob = clob;
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            try (Reader r = clob.getCharacterStream()) {
                new Utf8Encoder(out).encode(r);
                out.flush();
            } catch (SQLException e) {
                throw new CsvDiffException("reading the CLOB failed: " + e.getMessage(), e);
            }
        }

        /**
         * {@link Clob#length()} is characters, and the hint wants bytes, so this is
         * exact for ASCII and an undercount for everything else. That is the right
         * way round: an undercount reserves less than needed and grows, an
         * overcount by a factor of four would reserve 60 MB for a 15 MB column. A
         * driver that cannot answer cheaply, or at all, leaves it unknown.
         */
        @Override
        public long sizeHint() {
            try {
                return Math.max(clob.length(), 0);
            } catch (SQLException e) {
                return 0;
            }
        }
    }

    /**
     * The {@link #withHeader(Header, DiffSource)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class HeaderedSource implements DiffSource {
        private final Header header;
        private final DiffSource body;
        private final RowFeedOptions options;

        HeaderedSource(Header header, DiffSource body, RowFeedOptions options) {
            if (header == null) {
                throw new CsvDiffException("withHeader needs a header, received null");
            }
            if (body == null) {
                throw new CsvDiffException("withHeader needs a source to write the header for");
            }
            if (body.header().isPresent()) {
                throw new CsvDiffException("withHeader writes the header rows for a source that"
                        + " has none, and " + body.getClass().getSimpleName() + " already declares"
                        + " one. Give the header to that source instead of wrapping it");
            }
            this.header = header;
            this.body = body;
            this.options = options;
        }

        /** The point of the class, per {@link DiffOptions#headerLayout(Header)}: the
         *  rows written and the rows parsed are the same number because they come
         *  from the same object. */
        @Override
        public Optional<Header> header() {
            return Optional.of(header);
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            new CsvWriter(out, options.delimiter(), options.quote()).writeRows(header.toRows());
            body.feed(out);
        }

        /**
         * Unknown when the body's size is, because a hint covering the header alone
         * would be worse than none: it would size the reservation to a few hundred
         * bytes and then grow from there.
         */
        @Override
        public long sizeHint() {
            long rest = body.sizeHint();
            return rest == 0 ? 0 : rest + headerBytes();
        }

        /** Close enough for a hint. Quoting a name that contains the delimiter adds
         *  a couple of bytes this does not count, on a figure whose whole job is to
         *  be roughly right before anything has been read. */
        private long headerBytes() {
            long n = 0;
            for (List<String> row : header.toRows()) {
                for (String cell : row) {
                    if (cell != null) n += Utf8Encoder.utf8Length(cell);
                }
                n += row.size(); // the delimiters, and the newline
            }
            return n;
        }
    }

    /**
     * The {@link #ofRows(Header, java.util.Iterator)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class RowsSource implements DiffSource {
        private final Header header;
        private final Iterator<? extends List<String>> rows;
        private final RowFeedOptions options;

        RowsSource(Header header, Iterator<? extends List<String>> rows, RowFeedOptions options) {
            this.header = header;
            this.rows = rows;
            this.options = options;
        }

        @Override
        public Optional<Header> header() {
            return Optional.of(header);
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            CsvWriter w = new CsvWriter(out, options.delimiter(), options.quote());
            w.writeRows(header.toRows());

            int inBatch = 0;
            while (rows.hasNext()) {
                List<String> row = rows.next();
                if (row.size() != header.columnCount()) {
                    throw new CsvDiffException("row " + (inBatch + 1) + " has " + row.size()
                            + " values but the header declares " + header.columnCount() + " columns");
                }
                w.writeRow(row);
                if (++inBatch >= options.batchRows()) {
                    out.flush();
                    inBatch = 0;
                }
            }
            out.flush();
        }
    }

    /**
     * The {@link #ofJsonRows(Header, java.io.Reader)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class JsonRowsSource implements DiffSource {
        private final Header header;
        private final Reader given;   // the caller's, never closed
        private final Clob clob;      // ours to open and to close, or null
        private final RowFeedOptions options;

        JsonRowsSource(Header header, Reader given, Clob clob, RowFeedOptions options) {
            if (header == null) {
                throw new CsvDiffException("a JSON row source needs a header: a JSON array"
                        + " carries no key markers, no types and no guaranteed column order");
            }
            if (given == null && clob == null) {
                throw new CsvDiffException("a JSON source needs a reader, received null");
            }
            this.header = header;
            this.given = given;
            this.clob = clob;
            this.options = options;
        }

        @Override
        public Optional<Header> header() {
            return Optional.of(header);
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            if (clob == null) {
                write(given, out);
                return;
            }
            try (Reader r = clob.getCharacterStream()) {
                write(r, out);
            } catch (SQLException e) {
                throw new CsvDiffException("reading the CLOB failed: " + e.getMessage(), e);
            }
        }

        /** Straight into the row feed, so a JSON array and a result set differ only
         *  in where the values came from. */
        private void write(Reader json, ByteFeed out) throws IOException {
            new RowsSource(header, new JsonRows(header, json), options).feed(out);
        }
    }

    /**
     * The {@link #ofResultSet(java.sql.ResultSet, Header)} implementation. Reached through that factory,
     * never named by a caller.
     *
     * @hidden
     */
    final class ResultSetSource implements DiffSource {
        private final ResultSet rs;
        private final Header header;
        private final RowFeedOptions options;

        ResultSetSource(ResultSet rs, Header header, RowFeedOptions options) {
            this.rs = rs;
            this.header = header;
            this.options = options;
        }

        @Override
        public Optional<Header> header() {
            return Optional.of(header);
        }

        @Override
        public void feed(ByteFeed out) throws IOException {
            CsvWriter w = new CsvWriter(out, options.delimiter(), options.quote());
            w.writeRows(header.toRows());

            final int n = header.columnCount();
            try {
                checkWidth(n);
                int inBatch = 0;
                while (rs.next()) {
                    for (int c = 1; c <= n; c++) {
                        w.startValue(c - 1);
                        w.writeValue(SqlValues.render(rs, c));
                    }
                    w.endRow();
                    if (++inBatch >= options.batchRows()) {
                        out.flush();
                        inBatch = 0;
                    }
                }
                out.flush();
            } catch (SQLException e) {
                throw new CsvDiffException("reading the result set failed: " + e.getMessage(), e);
            }
        }

        /**
         * The one thing checked against the result set's own metadata, and the
         * reason it is checked rather than trusted: a header that is one column
         * narrower than the query parses as a ragged row hundreds of thousands of
         * rows later, and the error names a row number rather than the mistake.
         */
        private void checkWidth(int declared) throws SQLException {
            int actual = rs.getMetaData().getColumnCount();
            if (actual != declared) {
                throw new CsvDiffException("the header declares " + declared
                        + " columns but the result set has " + actual);
            }
        }
    }
}
