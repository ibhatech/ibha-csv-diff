package com.ibhatech.csvdiff.jni;

/**
 * Strict UTF-8 validation, matching the engine's.
 *
 * <p>It exists so that a decoded cell can report {@code invalidUtf8} the way the
 * JSONL emitter does. Decoding first and looking for U+FFFD afterwards cannot tell a
 * replacement from a replacement character the file genuinely contained, and a
 * report that quietly claims a customer's data was mojibake when it was not is the
 * sort of thing that costs an afternoon.
 *
 * <p>Strict means what the engine means and what {@code java.nio.charset} means:
 * overlong encodings, surrogate halves and anything above U+10FFFF are invalid, not
 * merely unusual.
 */
final class Utf8 {

    private Utf8() {
    }

    static boolean isValid(byte[] b, int len) {
        int i = 0;
        while (i < len) {
            int c = b[i] & 0xff;
            if (c < 0x80) {
                i++;
                continue;
            }
            int n;
            if (c >= 0xc2 && c <= 0xdf) {
                n = 2;
            } else if (c >= 0xe0 && c <= 0xef) {
                n = 3;
            } else if (c >= 0xf0 && c <= 0xf4) {
                n = 4;
            } else {
                return false; // a continuation byte on its own, or 0xc0/0xc1/0xf5+
            }
            if (i + n > len) return false;

            int c1 = b[i + 1] & 0xff;
            if ((c1 & 0xc0) != 0x80) return false;
            if (n == 3) {
                if (c == 0xe0 && c1 < 0xa0) return false; // overlong
                if (c == 0xed && c1 > 0x9f) return false; // a surrogate half
            }
            if (n == 4) {
                if (c == 0xf0 && c1 < 0x90) return false; // overlong
                if (c == 0xf4 && c1 > 0x8f) return false; // above U+10FFFF
            }
            for (int k = 2; k < n; k++) {
                if ((b[i + k] & 0xc0) != 0x80) return false;
            }
            i += n;
        }
        return true;
    }
}
