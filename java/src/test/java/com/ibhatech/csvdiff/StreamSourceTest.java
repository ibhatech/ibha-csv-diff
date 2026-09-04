package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayInputStream;
import java.io.InputStream;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The stream source, and the size hint a caller can now give it.
 *
 * <p>The hint halves what the engine reserves, because without it the arrays double
 * as they grow and the arena does not reclaim the abandoned copies. The streams a
 * server feeds this nearly all know their size, so the only thing standing between
 * them and the file path's reservation profile was somewhere to say so.
 *
 * <p>These assert the contract rather than the allocation: a hint changes how much
 * the engine reserves and never what it reports, which is exactly why a wrong one is
 * safe and why that has to be checked rather than assumed.
 */
class StreamSourceTest {

    private static byte[] source() {
        return Fixtures.bytes("tiny_source.csv");
    }

    private static byte[] target() {
        return Fixtures.bytes("tiny_target.csv");
    }

    private static InputStream stream(byte[] bytes) {
        return new ByteArrayInputStream(bytes);
    }

    /* ----------------------------------------------------------- the hint -- */

    @Test
    void aStreamWithNoHintReportsNone() {
        assertEquals(0, DiffSource.ofStream(stream(source())).sizeHint());
    }

    @Test
    void aStreamCarriesTheHintItWasGiven() {
        byte[] bytes = source();
        assertEquals(bytes.length, DiffSource.ofStream(stream(bytes), bytes.length).sizeHint());
    }

    @Test
    void zeroIsStillUnknown() {
        assertEquals(0, DiffSource.ofStream(stream(source()), 0).sizeHint());
    }

    @Test
    void aNegativeHintIsRejected() {
        CsvDiffException e = assertThrows(CsvDiffException.class,
                () -> DiffSource.ofStream(stream(source()), -1));
        assertEquals("a size hint cannot be negative, received -1", e.getMessage());
    }

    /* -------------------------------------------------- the report is equal -- */

    @Test
    void aHintedStreamReportsWhatTheBytesDo() {
        byte[] src = source();
        byte[] tgt = target();

        List<String> viaBytes;
        try (Diff d = CsvDiff.compare(DiffSource.ofBytes(src), DiffSource.ofBytes(tgt))) {
            viaBytes = describe(d);
        }

        List<String> viaStream;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src), src.length),
                DiffSource.ofStream(stream(tgt), tgt.length))) {
            viaStream = describe(d);
        }

        assertEquals(viaBytes, viaStream);
    }

    /**
     * The load bearing one. The hint sizes a reservation and is never checked
     * against what arrives, so a stale Content-Length or an approximate figure has
     * to cost performance and nothing else. A hint that truncated the read, or that
     * had to be right, would be a limit wearing a hint's name.
     */
    @Test
    void aWrongHintChangesNothingAboutTheReport() {
        byte[] src = source();
        byte[] tgt = target();

        List<String> correct;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src), src.length),
                DiffSource.ofStream(stream(tgt), tgt.length))) {
            correct = describe(d);
        }

        // far too small: the engine reserves once and then grows as it always did
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src), 8),
                DiffSource.ofStream(stream(tgt), 8))) {
            assertEquals(correct, describe(d), "a hint under the real size");
        }

        // far too large: it over reserves and reads exactly as many bytes as arrive
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src), 4L * 1024 * 1024),
                DiffSource.ofStream(stream(tgt), 4L * 1024 * 1024))) {
            assertEquals(correct, describe(d), "a hint over the real size");
        }
    }

    @Test
    void anUnhintedStreamStillWorks() {
        byte[] src = source();
        byte[] tgt = target();

        List<String> hinted;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src), src.length),
                DiffSource.ofStream(stream(tgt), tgt.length))) {
            hinted = describe(d);
        }

        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src)), DiffSource.ofStream(stream(tgt)))) {
            assertEquals(hinted, describe(d));
        }
    }

    /* ------------------------------------------------------- the effect -- */

    /**
     * The reason the overload exists, measured rather than taken from the
     * documentation. It came out at <strong>86 MB hinted against 181 MB unhinted, a
     * factor of 2.1</strong>, which is smaller than the 4x the javadoc claimed
     * before this was run, and the claim was corrected rather than the measurement
     * dropped.
     *
     * <p>The assertion is the direction, not the ratio: the exact figure depends on
     * how the arena happens to double for a given pair, and a test that pinned it
     * would fail on a fixture change for no reason anyone could act on.
     */
    @Test
    void aHintReservesLessThanNoHint() {
        org.junit.jupiter.api.Assumptions.assumeTrue(Fixtures.hasP90(),
                "needs the 15 MB pair from core/fixtures/generated");
        byte[] src = Fixtures.bytes("p90_source.csv");
        byte[] tgt = Fixtures.bytes("p90_target.csv");

        long unhinted;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src)), DiffSource.ofStream(stream(tgt)))) {
            unhinted = d.bytesReserved();
        }

        long hinted;
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(stream(src), src.length),
                DiffSource.ofStream(stream(tgt), tgt.length))) {
            hinted = d.bytesReserved();
        }

        System.out.println("p90 bytes reserved: hinted " + hinted + ", unhinted " + unhinted
                + ", ratio " + String.format("%.2f", (double) unhinted / hinted));
        assertTrue(hinted < unhinted,
                "expected the hint to reserve less: hinted " + hinted + ", unhinted " + unhinted);
    }

    /* ------------------------------------------------------- the lifecycle -- */

    /** "Closed by the caller, not by this" is a documented contract, and a source
     *  that helpfully closed the stream would break a caller reading two sides out
     *  of one archive or one connection. */
    @Test
    void theStreamIsNotClosedByTheSource() {
        CloseWatcher src = new CloseWatcher(source());
        CloseWatcher tgt = new CloseWatcher(target());
        try (Diff d = CsvDiff.compare(
                DiffSource.ofStream(src, source().length),
                DiffSource.ofStream(tgt, target().length))) {
            d.summary();
        }
        assertFalse(src.closed, "the source side's stream was closed");
        assertFalse(tgt.closed, "the target side's stream was closed");
    }

    private static final class CloseWatcher extends ByteArrayInputStream {
        boolean closed;

        CloseWatcher(byte[] bytes) {
            super(bytes);
        }

        @Override
        public void close() {
            closed = true;
        }
    }

    /* ---------------------------------------------------------------- utils -- */

    /** The whole report as text, so a difference anywhere shows up as one failure
     *  rather than as a count that happens to match. */
    private static List<String> describe(Diff d) {
        return d.rows()
                .map(r -> r.kind() + " " + r.sourceRow() + "->" + r.targetRow() + " "
                        + r.cells().stream()
                                .map(c -> c.source().orElse("<none>") + "|" + c.target().orElse("<none>"))
                                .toList())
                .toList();
    }
}
