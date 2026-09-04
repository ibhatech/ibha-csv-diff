package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.Reader;
import java.io.StringReader;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.sql.Clob;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The character sources: {@code ofString}, {@code ofReader}, {@code ofClob}.
 *
 * <p>What has to hold is that text reaches the engine as exactly the bytes
 * {@code getBytes(UTF_8)} would have produced, whichever factory it came through and
 * wherever the chunk boundaries happen to fall. The bytes are asserted directly
 * rather than through a report, because a report can agree while the encoding is
 * wrong: a corrupted character on both sides compares equal to itself.
 *
 * <p>The boundary that matters is the surrogate pair. A character above U+FFFF is
 * two chars whose four UTF-8 bytes cannot come from either half alone, so a read
 * that ends between them has to hold the first back. These sweep it across the
 * chunk boundary rather than testing one placement, because the failure is an
 * off by one and one placement is exactly what an off by one survives.
 */
class CharacterSourceTest {

    private static final String EMOJI = "😀";     // U+1F600, four UTF-8 bytes

    /** The four canonical header rows and four data rows, so the comparison is keyed
     *  and a changed cell reports as a changed cell rather than through the all-keys
     *  similarity path. Every width of UTF-8 sequence appears in it: one byte, two,
     *  three, and the four byte pair that is two Java chars. */
    private static final String MIXED = """
            KEY,
            REQUIRED,
            INTEGER,VARCHAR(20)
            id,name
            1,Ann
            2,Björn
            3,中文
            4,""" + EMOJI + "\n";

    /* ------------------------------------------------------------- the bytes -- */

    @Test
    void aStringEncodesToExactlyWhatGetBytesWouldProduce() throws IOException {
        assertArrayEquals(MIXED.getBytes(StandardCharsets.UTF_8),
                fed(DiffSource.ofString(MIXED), 1 << 12));
    }

    @Test
    void aReaderEncodesToTheSameBytesAsTheString() throws IOException {
        assertArrayEquals(MIXED.getBytes(StandardCharsets.UTF_8),
                fed(DiffSource.ofReader(new StringReader(MIXED)), 1 << 12));
    }

    /**
     * A staging buffer of 7 bytes cannot hold the four byte character whenever it
     * has fewer than four left, so the encoder has to flush and carry on rather than
     * split the sequence. Seven is deliberately not a multiple of anything.
     */
    @Test
    void aByteBufferTooSmallToHoldTheCharacterStillEncodesIt() throws IOException {
        assertArrayEquals(MIXED.getBytes(StandardCharsets.UTF_8),
                fed(DiffSource.ofString(MIXED), 7));
    }

    /**
     * The one the whole class exists for. The pair lands on the read boundary at one
     * exact offset and either side of it, and the naive loop, which flips and clears
     * instead of compacting, emits a replacement character at precisely that offset
     * and nowhere else.
     */
    @Test
    void aSurrogatePairAcrossTheReadBoundaryStaysOneCharacter() throws IOException {
        for (int at = Utf8Encoder.CHUNK - 3; at <= Utf8Encoder.CHUNK + 1; at++) {
            String text = "a".repeat(at) + EMOJI + "b".repeat(16);
            assertArrayEquals(text.getBytes(StandardCharsets.UTF_8),
                    fed(DiffSource.ofReader(new StringReader(text)), 1 << 12),
                    "the pair starting at char " + at + ", boundary " + Utf8Encoder.CHUNK);
        }
    }

    /**
     * A reader that hands over one character per call splits every pair there is, so
     * this is the same boundary at every offset at once. Real ones do this: a CLOB
     * reader returns what the driver has buffered, not what was asked for.
     */
    @Test
    void aReaderThatDripsOneCharAtATimeEncodesTheSameBytes() throws IOException {
        assertArrayEquals(MIXED.getBytes(StandardCharsets.UTF_8),
                fed(DiffSource.ofReader(new DripReader(MIXED)), 1 << 12));
    }

    /**
     * An unpaired surrogate is not text and cannot be encoded. It is replaced rather
     * than refused because {@code String.getBytes(UTF_8)} replaces it and so does
     * {@link CsvWriter} on a row value: the same text has to become the same bytes
     * through every factory, and a source that threw where the row feed substituted
     * would be a comparison that depends on which one the caller reached for.
     */
    @Test
    void anUnpairedSurrogateIsReplacedExactlyAsGetBytesReplacesIt() throws IOException {
        String lone = "a\uD83Db";                              // a high surrogate, no partner
        byte[] expected = lone.getBytes(StandardCharsets.UTF_8);
        assertArrayEquals(expected, fed(DiffSource.ofString(lone), 1 << 12));
        assertArrayEquals(expected, fed(DiffSource.ofReader(new StringReader(lone)), 1 << 12));
        assertArrayEquals(expected, fed(DiffSource.ofReader(new DripReader(lone)), 1 << 12));
    }

    @Test
    void anEmptySourceWritesNothing() throws IOException {
        assertArrayEquals(new byte[0], fed(DiffSource.ofString(""), 1 << 12));
        assertArrayEquals(new byte[0], fed(DiffSource.ofReader(new StringReader("")), 1 << 12));
    }

    /* ------------------------------------------------------------ the report -- */

    @Test
    void aTextSourceReportsWhatTheEquivalentBytesDo() {
        String src = MIXED;
        String tgt = MIXED.replace("Ann", "Anne");

        List<String> viaBytes;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofBytes(src.getBytes(StandardCharsets.UTF_8)),
                DiffSource.ofBytes(tgt.getBytes(StandardCharsets.UTF_8)),
                DiffOptions.defaults())) {
            viaBytes = describe(d);
        }

        List<String> viaText;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofString(src), DiffSource.ofString(tgt), DiffOptions.defaults())) {
            viaText = describe(d);
        }

        List<String> viaReader;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofReader(new DripReader(src)),
                DiffSource.ofReader(new StringReader(tgt)),
                DiffOptions.defaults())) {
            viaReader = describe(d);
        }

        assertEquals(viaBytes, viaText);
        assertEquals(viaBytes, viaReader);
        assertTrue(viaBytes.stream().anyMatch(r -> r.contains("Ann|Anne")), viaBytes.toString());
    }

    /** The non-ASCII cells have to survive as far as the report, not only as far as
     *  the parser. A single byte encoding would compare clean and hand back mojibake. */
    @Test
    void nonAsciiTextSurvivesIntoTheReport() {
        try (Diff d = CsvDiff.compare(
                DiffSource.ofString(MIXED),
                DiffSource.ofString(MIXED.replace(EMOJI, "é")),
                DiffOptions.defaults())) {
            List<String> rows = describe(d);
            assertTrue(rows.stream().anyMatch(r -> r.contains(EMOJI + "|é")), rows.toString());
        }
    }

    /* -------------------------------------------------------------- the clob -- */

    @Test
    void aClobReadsThroughItsCharacterStream() {
        FakeClob src = new FakeClob(MIXED);
        FakeClob tgt = new FakeClob(MIXED.replace("Ann", "Anne"));

        List<String> viaClob;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofClob(src.proxy()), DiffSource.ofClob(tgt.proxy()), DiffOptions.defaults())) {
            viaClob = describe(d);
        }

        List<String> viaText;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofString(MIXED),
                DiffSource.ofString(MIXED.replace("Ann", "Anne")),
                DiffOptions.defaults())) {
            viaText = describe(d);
        }

        assertEquals(viaText, viaClob);
        assertTrue(src.asciiStreamAsked == 0, "getAsciiStream would have corrupted the text");
    }

    /** This one opened the stream, so this one closes it. The CLOB itself belongs to
     *  the caller and may be read again, so it is not freed. */
    @Test
    void theClobStreamIsClosedAndTheClobIsNot() {
        FakeClob clob = new FakeClob(MIXED);
        try (Diff d = CsvDiff.compare(
                DiffSource.ofClob(clob.proxy()), DiffSource.ofString(MIXED), DiffOptions.defaults())) {
            d.summary();
        }
        assertTrue(clob.streams.get(0).closed, "the character stream was left open");
        assertFalse(clob.freed, "the caller's CLOB was freed");
    }

    /** The opposite: a reader the caller handed in is the caller's, exactly as
     *  {@code ofStream} leaves an InputStream open. */
    @Test
    void aReaderIsNotClosedByTheSource() {
        CloseWatcher r = new CloseWatcher(MIXED);
        try (Diff d = CsvDiff.compare(
                DiffSource.ofReader(r), DiffSource.ofString(MIXED), DiffOptions.defaults())) {
            d.summary();
        }
        assertFalse(r.closed, "the caller's reader was closed");
    }

    /* --------------------------------------------------------- the size hint -- */

    @Test
    void aStringKnowsItsExactEncodedLength() {
        assertEquals(MIXED.getBytes(StandardCharsets.UTF_8).length,
                DiffSource.ofString(MIXED).sizeHint());
        assertEquals(0, DiffSource.ofString("").sizeHint());
        for (String s : List.of("plain", "é", "中文", EMOJI, "a\uD83Db", MIXED)) {
            assertEquals(s.getBytes(StandardCharsets.UTF_8).length,
                    DiffSource.ofString(s).sizeHint(), "the encoded length of " + s.length()
                            + " chars");
        }
    }

    @Test
    void aReaderCarriesTheHintItWasGivenAndNoneOtherwise() {
        assertEquals(0, DiffSource.ofReader(new StringReader(MIXED)).sizeHint());
        assertEquals(4096, DiffSource.ofReader(new StringReader(MIXED), 4096).sizeHint());
        CsvDiffException e = assertThrows(CsvDiffException.class,
                () -> DiffSource.ofReader(new StringReader(MIXED), -1));
        assertEquals("a size hint cannot be negative, received -1", e.getMessage());
    }

    /** Characters, which is what a driver can answer cheaply, and an undercount for
     *  anything but ASCII. Undercounting reserves less than needed and grows;
     *  overcounting by four would reserve 60 MB for a 15 MB column. */
    @Test
    void aClobHintsWithItsCharacterLength() {
        assertEquals(MIXED.length(), DiffSource.ofClob(new FakeClob(MIXED).proxy()).sizeHint());
        assertTrue(DiffSource.ofClob(new FakeClob(MIXED).proxy()).sizeHint()
                <= MIXED.getBytes(StandardCharsets.UTF_8).length, "a hint that overshoots");
    }

    /**
     * The exact hint is worth having, and this is the shape of the proof: on the
     * 15 MB pair a text source reserves what the byte source reserves, to the byte.
     * {@code StreamSourceTest} measured what the absence of a hint costs, 181 MB
     * against 86 MB; this says the character path does not pay it.
     */
    @Test
    void aTextSourceReservesWhatTheEquivalentBytesDo() {
        org.junit.jupiter.api.Assumptions.assumeTrue(Fixtures.hasP90(),
                "needs the 15 MB pair from core/fixtures/generated");
        byte[] src = Fixtures.bytes("p90_source.csv");
        byte[] tgt = Fixtures.bytes("p90_target.csv");

        long viaBytes;
        try (Diff d = CsvDiff.compare(DiffSource.ofBytes(src), DiffSource.ofBytes(tgt))) {
            viaBytes = d.bytesReserved();
        }

        long viaText;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofString(new String(src, StandardCharsets.UTF_8)),
                DiffSource.ofString(new String(tgt, StandardCharsets.UTF_8)))) {
            viaText = d.bytesReserved();
        }

        assertEquals(viaBytes, viaText, "a text source should reserve what the bytes do");
    }

    /* ------------------------------------------------------- the null checks -- */

    @Test
    void aNullSourceIsRejectedWhereItIsGivenRatherThanWhereItIsRead() {
        assertThrows(CsvDiffException.class, () -> DiffSource.ofString(null));
        assertThrows(CsvDiffException.class, () -> DiffSource.ofClob(null));
    }

    @Test
    void aTextSourceDeclaresNoHeaderOfItsOwn() {
        assertTrue(DiffSource.ofString(MIXED).header().isEmpty());
        assertTrue(DiffSource.ofReader(new StringReader(MIXED)).header().isEmpty());
        assertTrue(DiffSource.ofClob(new FakeClob(MIXED).proxy()).header().isEmpty());
    }

    /* ---------------------------------------------------------------- utils -- */

    /** Everything a source writes, through a feed of the given capacity, so that the
     *  encoding is asserted rather than inferred from a report agreeing with itself. */
    private static byte[] fed(DiffSource source, int capacity) throws IOException {
        Capture c = new Capture(capacity);
        source.feed(c);
        return c.bytes();
    }

    private static List<String> describe(Diff d) {
        return d.rows()
                .map(r -> r.kind() + " " + r.sourceRow() + "->" + r.targetRow() + " "
                        + r.cells().stream()
                                .map(c -> c.source().orElse("<none>") + "|" + c.target().orElse("<none>"))
                                .toList())
                .toList();
    }

    /** A {@link ByteFeed} that keeps what was written. Direct, like the real one, so
     *  the encoder is writing into the same kind of buffer it will meet in anger. */
    private static final class Capture implements ByteFeed {
        private final ByteBuffer buffer;
        private final ByteArrayOutputStream out = new ByteArrayOutputStream();

        Capture(int capacity) {
            this.buffer = ByteBuffer.allocateDirect(capacity);
        }

        @Override
        public ByteBuffer buffer() {
            return buffer;
        }

        @Override
        public void flush() {
            buffer.flip();
            byte[] b = new byte[buffer.remaining()];
            buffer.get(b);
            out.write(b, 0, b.length);
            buffer.clear();
        }

        byte[] bytes() {
            return out.toByteArray();
        }
    }

    /** One character per read, which splits every surrogate pair in the text. */
    private static final class DripReader extends Reader {
        private final String text;
        private int at;

        DripReader(String text) {
            this.text = text;
        }

        @Override
        public int read(char[] buf, int off, int len) {
            if (at >= text.length()) return -1;
            if (len == 0) return 0;
            buf[off] = text.charAt(at++);
            return 1;
        }

        @Override
        public void close() {
        }
    }

    private static final class CloseWatcher extends StringReader {
        boolean closed;

        CloseWatcher(String s) {
            super(s);
        }

        @Override
        public void close() {
            closed = true;
            super.close();
        }
    }

    /**
     * A {@link Clob} over a string, built as a proxy for the reason
     * {@link FakeResultSet} is one: the interface has a dozen methods and a stub of
     * it would bury the three that matter in a wall of {@code throw}.
     */
    private static final class FakeClob implements InvocationHandler {
        private final String text;
        final List<Watched> streams = new ArrayList<>();
        int asciiStreamAsked;
        boolean freed;

        FakeClob(String text) {
            this.text = text;
        }

        Clob proxy() {
            return (Clob) Proxy.newProxyInstance(CharacterSourceTest.class.getClassLoader(),
                    new Class<?>[] {Clob.class}, this);
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) {
            switch (method.getName()) {
                case "length":
                    return (long) text.length();
                case "getCharacterStream": {
                    Watched w = new Watched(text);
                    streams.add(w);
                    return w;
                }
                case "getAsciiStream":
                    asciiStreamAsked++;
                    throw new UnsupportedOperationException("getAsciiStream corrupts the text");
                case "free":
                    freed = true;
                    return null;
                case "toString":
                    return "FakeClob(" + text.length() + " chars)";
                case "hashCode":
                    return System.identityHashCode(proxy);
                case "equals":
                    return proxy == args[0];
                default:
                    throw new UnsupportedOperationException(method.getName());
            }
        }

        static final class Watched extends StringReader {
            boolean closed;

            Watched(String s) {
                super(s);
            }

            @Override
            public void close() {
                closed = true;
                super.close();
            }
        }
    }
}
