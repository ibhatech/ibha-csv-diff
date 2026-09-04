package com.ibhatech.csvdiff;

import java.io.IOException;
import java.io.Reader;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharsetEncoder;
import java.nio.charset.CoderResult;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

/**
 * Turns characters into the UTF-8 the engine reads, straight into the staging
 * buffer.
 *
 * <p><strong>Why this exists rather than {@code text.getBytes(UTF_8)}.</strong> A
 * CLOB is character data and every other source here is byte oriented, so a caller
 * comparing one has two choices today: {@code clob.getAsciiStream()}, which corrupts
 * every non-ASCII byte, or {@code rs.getString(n).getBytes(UTF_8)}, which is correct
 * but holds the text three times at peak, as the driver's {@code String}, as the
 * encoded {@code byte[]}, and again as the engine's arena copy. On a 15 MB table
 * that is 30 MB of char data plus a 15 MB array, live at the same moment, per side.
 * This encodes in fixed size chunks directly into {@link ByteFeed#buffer()}, which
 * is the direct buffer the engine reads from, so the only steady state cost is one
 * chunk of chars.
 *
 * <p><strong>The surrogate pair is the whole difficulty.</strong> A character above
 * U+FFFF is two Java {@code char}s and its four UTF-8 bytes cannot be produced from
 * either half alone, so a chunk boundary that falls between them has to hold the
 * high surrogate back until the next read arrives. {@link CharBuffer#compact()} is
 * what keeps it: the encoder reports {@code UNDERFLOW} with the lone surrogate still
 * unconsumed, compact moves it to the front, and the next read appends its partner.
 * A naive loop that flipped and cleared instead would emit a replacement character
 * in the middle of the caller's data and never say so.
 *
 * <p>Malformed input, which at this level means an unpaired surrogate that no later
 * read completes, is <strong>replaced</strong> rather than rejected, because that is
 * exactly what {@code String.getBytes(UTF_8)} does and what {@link CsvWriter} does
 * to a row value. A given piece of text has to reach the engine as the same bytes
 * whichever factory the caller picked, and a source that threw where the row feed
 * substituted would break that.
 */
final class Utf8Encoder {

    /**
     * Characters read per pass. 8,192 chars is 16 KB of char data, which is small
     * next to the staging buffer it feeds and large enough that the per read
     * overhead of a {@code Reader} over a network CLOB is not what dominates.
     *
     * <p>Package private so the test can put a surrogate pair exactly on the
     * boundary rather than on a number copied out of here and left behind when this
     * one changes.
     */
    static final int CHUNK = 8192;

    private final ByteFeed out;
    private final CharsetEncoder encoder = StandardCharsets.UTF_8.newEncoder()
            .onMalformedInput(CodingErrorAction.REPLACE)
            .onUnmappableCharacter(CodingErrorAction.REPLACE);

    Utf8Encoder(ByteFeed out) {
        this.out = out;
    }

    /**
     * Encodes everything the reader has. Does not close it and does not flush the
     * feed: the caller owns both, because a composed source writes more bytes after
     * this returns.
     */
    void encode(Reader in) throws IOException {
        CharBuffer chars = CharBuffer.allocate(CHUNK);
        ByteBuffer bytes = out.buffer();
        for (;;) {
            int n = in.read(chars);
            boolean end = n < 0;
            chars.flip();
            bytes = drain(chars, bytes, end);
            if (end) break;
            // Keeps a high surrogate the encoder could not use, so the low surrogate
            // of the next read lands next to it rather than after a replacement.
            chars.compact();
        }
        finish(bytes);
    }

    /**
     * Encodes a sequence the caller already holds in memory, without copying it.
     * {@link CharBuffer#wrap} is a view, so a 15 MB {@code String} is not doubled on
     * its way in.
     */
    void encode(CharSequence text) throws IOException {
        finish(drain(CharBuffer.wrap(text), out.buffer(), true));
    }

    /**
     * Encodes until the input is exhausted, flushing the feed whenever the staging
     * buffer fills. Returns the buffer to keep writing into, which is a different
     * object identity's worth of state after a flush.
     */
    private ByteBuffer drain(CharBuffer in, ByteBuffer out0, boolean end) throws IOException {
        ByteBuffer bytes = out0;
        for (;;) {
            CoderResult r = encoder.encode(in, bytes, end);
            if (r.isUnderflow()) return bytes;
            if (r.isOverflow()) {
                out.flush();
                bytes = out.buffer();
                continue;
            }
            // Unreachable while both error actions are REPLACE, and left in place
            // because a future change to either one must not fall through silently.
            throw new CsvDiffException("could not encode the text as UTF-8: " + r);
        }
    }

    /** Drains whatever state the encoder itself is still holding. */
    private void finish(ByteBuffer out0) throws IOException {
        ByteBuffer bytes = out0;
        for (;;) {
            CoderResult r = encoder.flush(bytes);
            if (r.isUnderflow()) return;
            out.flush();
            bytes = out.buffer();
        }
    }

    /**
     * How many bytes {@link #encode(CharSequence)} will write, exactly.
     *
     * <p>One pass over the chars and no allocation, which is what makes it worth
     * doing at all: it turns a size hint from a guess into the real figure, and the
     * hint is what decides whether the engine reserves once or doubles its arrays as
     * it grows. An unpaired surrogate counts as one byte because that is the width
     * of the replacement it encodes to.
     */
    static long utf8Length(CharSequence s) {
        long n = 0;
        int len = s.length();
        for (int i = 0; i < len; i++) {
            char c = s.charAt(i);
            if (c < 0x80) {
                n += 1;
            } else if (c < 0x800) {
                n += 2;
            } else if (Character.isHighSurrogate(c) && i + 1 < len
                    && Character.isLowSurrogate(s.charAt(i + 1))) {
                n += 4;
                i++;
            } else if (Character.isSurrogate(c)) {
                n += 1;
            } else {
                n += 3;
            }
        }
        return n;
    }
}
