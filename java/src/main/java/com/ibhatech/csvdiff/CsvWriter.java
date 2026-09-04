package com.ibhatech.csvdiff;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * Writes rows as RFC 4180 CSV into a {@link ByteFeed}.
 *
 * <p>This is the whole of the row feed. A {@code ResultSet} becomes bytes in the
 * dialect the engine already parses, rather than a second front end into the
 * columnar index, so a result set and a file are the same input to the same state
 * machine and cannot possibly be diffed under different rules. It costs one escape
 * pass over the values, which is cheap next to the guarantee.
 *
 * <p>Quoting follows the engine's own reading of RFC 4180: a value is quoted when it
 * contains the delimiter, a quote, a CR or an LF, and a quote inside a quoted value
 * is doubled. Quoting is never a difference to the engine, per spec 5.2, so
 * over-quoting would be harmless; under-quoting would corrupt the row.
 *
 * <p>There is deliberately no formula guard here. That guard exists on the CSV
 * <em>emitter</em>, where the output is a file someone opens in Excel. These bytes
 * are consumed by the parser in this process and never written anywhere, so
 * prefixing values would change what gets compared, which is the one thing an
 * input path must not do.
 */
final class CsvWriter {

    private final ByteFeed out;
    private final byte delimiter;
    private final byte quote;

    CsvWriter(ByteFeed out, byte delimiter, byte quote) {
        this.out = out;
        this.delimiter = delimiter;
        this.quote = quote;
    }

    void writeRows(List<? extends List<String>> rows) throws IOException {
        for (List<String> row : rows) writeRow(row);
    }

    void writeRow(List<String> values) throws IOException {
        for (int i = 0; i < values.size(); i++) {
            if (i > 0) out.write(delimiter);
            writeValue(values.get(i));
        }
        out.write((byte) '\n');
    }

    /** Starts a row written value by value; call {@link #endRow()} after the last. */
    void startValue(int index) throws IOException {
        if (index > 0) out.write(delimiter);
    }

    void endRow() throws IOException {
        out.write((byte) '\n');
    }

    void writeValue(String value) throws IOException {
        if (value == null || value.isEmpty()) return;
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (!needsQuoting(bytes)) {
            out.write(bytes, 0, bytes.length);
            return;
        }
        out.write(quote);
        int start = 0;
        for (int i = 0; i < bytes.length; i++) {
            if (bytes[i] != quote) continue;
            out.write(bytes, start, i - start + 1);
            out.write(quote); // the doubled quote of RFC 4180
            start = i + 1;
        }
        out.write(bytes, start, bytes.length - start);
        out.write(quote);
    }

    private boolean needsQuoting(byte[] bytes) {
        for (byte b : bytes) {
            if (b == delimiter || b == quote || b == '\n' || b == '\r') return true;
        }
        return false;
    }
}
