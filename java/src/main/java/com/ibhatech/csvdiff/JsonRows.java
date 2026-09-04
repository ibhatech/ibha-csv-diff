package com.ibhatech.csvdiff;

import java.io.IOException;
import java.io.Reader;
import java.sql.SQLException;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.NoSuchElementException;

/**
 * A JSON array of objects, pulled one row at a time.
 *
 * <p><strong>Why not {@link Json}.</strong> That one parses a whole document into
 * {@code Map} and {@code List} objects and exists to read a summary this library
 * produced itself, which is a few hundred bytes. A 15 MB row array through it would
 * allocate millions of objects, which is precisely the heap behaviour the C engine
 * was chosen to avoid. This reads one object at a time into a reused buffer and hands
 * out one row, so a document of any size costs one row of live data.
 *
 * <p><strong>Array of objects, and only that.</strong>
 * {@code [{"id":"1","name":"a"}, ...]}, which is what an API emits. The array of
 * arrays shape, {@code [["1","a"]]}, was considered and deliberately not built: the
 * deployment does not hold it, and a shape nobody has is a parser nobody tests.
 *
 * <h2>The rules, and why each one is what it is</h2>
 *
 * <p><strong>The header decides the column order</strong>, not the order keys happen
 * to appear in. JSON objects are unordered by definition and two rows of the same
 * export can list their keys differently, so each object is projected by name into
 * the {@link Header}'s order.
 *
 * <p><strong>A key the header declares and the object does not carry is an error</strong>,
 * and so is a key the header does not declare. An empty cell for the first would
 * report as a changed value against a file that has the real one, which is a
 * difference nobody made; and the second is the same disagreement in the other
 * direction, the JSON analogue of an added column, which the CSV path also refuses
 * by default. The same key twice is an error for the same reason: JSON says nothing
 * about which one wins, so neither do we.
 *
 * <p><strong>Values are rendered by {@link SqlValues}</strong>, the one already used
 * for JDBC, rather than by a second renderer here. That is not tidiness: use case 3
 * compares a JSON side against a JDBC side of the same data, and two renderers would
 * eventually disagree about a boolean or a null and report a difference in the data
 * that is really a difference in this library. So {@code null} becomes an empty
 * field, {@code true} becomes {@code TRUE}, and a string is itself.
 *
 * <p><strong>A number keeps its literal text</strong> and never goes through a
 * {@code double}. {@code 1.50} has to survive as {@code 1.50} for a
 * {@code DECIMAL(12,2)} comparison to mean anything, and a value wider than a
 * {@code double} has to survive at all. This is the same rule as
 * {@code BigDecimal.toPlainString()} in {@code SqlValues}, reached by not parsing
 * rather than by parsing carefully.
 *
 * <p><strong>A nested object or array is refused</strong>, as binary is refused in
 * {@code SqlValues} and for the same reason: a CSV cell has no structure, so any
 * rendering picked here would be one the file side did not use, and every such cell
 * would report as changed.
 */
final class JsonRows implements Iterator<List<String>> {

    private final Header header;
    private final Map<String, Integer> columnByName;
    private final Reader in;

    private final char[] buf = new char[8192];
    private int at;
    private int limit;
    private boolean eof;

    /** For error messages, which without a position are of no use at all on a
     *  document that is one line and 15 MB long. */
    private long offset;
    private long lineStart;
    private int line = 1;

    private final StringBuilder text = new StringBuilder(64);

    private boolean started;
    private boolean done;
    private boolean ready;
    private long row;

    JsonRows(Header header, Reader in) {
        this.header = header;
        this.in = in;
        this.columnByName = new HashMap<>();
        List<String> names = header.names();
        for (int i = 0; i < names.size(); i++) {
            // A header with a duplicate name is the caller's business and the engine
            // has its own opinion about it; here the first one wins, so that a
            // lookup is at least deterministic.
            columnByName.putIfAbsent(names.get(i), i);
        }
    }

    /* ------------------------------------------------------------- the rows -- */

    @Override
    public boolean hasNext() {
        if (done) return false;
        if (ready) return true;

        if (!started) {
            started = true;
            skipWhitespace();
            expect('[', "a JSON row array starts with [");
            skipWhitespace();
            if (peek() == ']') {
                take();
                return end();
            }
            ready = true;
            return true;
        }

        skipWhitespace();
        int c = take();
        if (c == ']') return end();
        if (c != ',') {
            throw error("expected , or ] between rows, found " + describe(c));
        }
        skipWhitespace();
        if (peek() == ']') throw error("a trailing comma after the last row");
        ready = true;
        return true;
    }

    @Override
    public List<String> next() {
        if (!hasNext()) throw new NoSuchElementException();
        ready = false;
        row++;
        return readRow();
    }

    /** Nothing but whitespace may follow the closing bracket. Two documents
     *  concatenated is otherwise a silently truncated comparison. */
    private boolean end() {
        skipWhitespace();
        if (peek() != -1) {
            throw error("content after the end of the row array, found " + describe(peek()));
        }
        done = true;
        return false;
    }

    private List<String> readRow() {
        skipWhitespace();
        expect('{', "row " + row + " is not a JSON object. This source reads an array of"
                + " objects, [{\"id\":\"1\"}, ...]");

        int n = header.columnCount();
        String[] cells = new String[n];
        boolean[] seen = new boolean[n];

        skipWhitespace();
        if (peek() == '}') {
            take();
        } else {
            for (;;) {
                skipWhitespace();
                expect('"', "row " + row + " has a member name that is not a string");
                String key = readString();

                Integer column = columnByName.get(key);
                if (column == null) {
                    throw error("row " + row + " has the key \"" + key + "\", which the header"
                            + " does not declare. The header names are " + header.names()
                            + ". A key the header does not carry is the JSON form of an added"
                            + " column, and comparing without it would compare a different table");
                }
                if (seen[column]) {
                    throw error("row " + row + " has the key \"" + key + "\" twice, and JSON does"
                            + " not say which one wins");
                }

                skipWhitespace();
                expect(':', "row " + row + ", after the key \"" + key + "\"");
                skipWhitespace();
                cells[column] = render(readValue(key), column);
                seen[column] = true;

                skipWhitespace();
                int c = take();
                if (c == '}') break;
                if (c != ',') {
                    throw error("expected , or } in row " + row + ", found " + describe(c));
                }
            }
        }

        for (int i = 0; i < n; i++) {
            if (!seen[i]) {
                throw error("row " + row + " has no value for the column \""
                        + header.names().get(i) + "\", which the header declares. An empty cell"
                        + " here would report as a changed value against a file that carries the"
                        + " real one, so the disagreement is reported where it is");
            }
        }
        return Arrays.asList(cells);
    }

    /* ----------------------------------------------------------- the values -- */

    /**
     * @return a {@code String} for a string or a number, a {@code Boolean}, or null
     *         for JSON null. Exactly the types {@link SqlValues} already renders.
     */
    private Object readValue(String key) {
        int c = peek();
        switch (c) {
            case '"':
                take();
                return readString();
            case 't':
                literal("true");
                return Boolean.TRUE;
            case 'f':
                literal("false");
                return Boolean.FALSE;
            case 'n':
                literal("null");
                return null;
            case '{':
            case '[':
                throw error("the value of \"" + key + "\" in row " + row + " is a nested "
                        + (c == '{' ? "object" : "array") + ", and a CSV cell has no structure."
                        + " Any rendering chosen here would be one the other side did not use,"
                        + " so project it in the query, or leave the column out of the header");
            case -1:
                throw error("the JSON ended in the middle of row " + row);
            default:
                return number(key);
        }
    }

    /** The literal text, exactly as written. Parsing it would lose the trailing zero
     *  of 1.50 and the low digits of anything wider than a double. */
    private String number(String key) {
        text.setLength(0);
        int c = peek();
        if (c != '-' && (c < '0' || c > '9')) {
            throw error("the value of \"" + key + "\" in row " + row + " is not a JSON value,"
                    + " found " + describe(c));
        }
        while (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || (c >= '0' && c <= '9')) {
            text.append((char) take());
            c = peek();
        }
        return text.toString();
    }

    /** The body of a string, the opening quote already taken. */
    private String readString() {
        text.setLength(0);
        for (;;) {
            int c = take();
            if (c == -1) throw error("the JSON ended inside a string");
            if (c == '"') return text.toString();
            if (c != '\\') {
                text.append((char) c);
                continue;
            }
            int e = take();
            switch (e) {
                case '"' -> text.append('"');
                case '\\' -> text.append('\\');
                case '/' -> text.append('/');
                case 'b' -> text.append('\b');
                case 'f' -> text.append('\f');
                case 'n' -> text.append('\n');
                case 'r' -> text.append('\r');
                case 't' -> text.append('\t');
                // Both halves of a surrogate pair arrive as their own \\u escape and
                // are appended as their own chars, which is what the pair already is
                // in Java. The encoder puts them back together.
                case 'u' -> text.append(hex4());
                case -1 -> throw error("the JSON ended inside a string escape");
                default -> throw error("unknown string escape \\" + (char) e + " in row " + row);
            }
        }
    }

    private char hex4() {
        int v = 0;
        for (int i = 0; i < 4; i++) {
            int c = take();
            int d = Character.digit(c, 16);
            if (d < 0) throw error("a \\u escape needs four hex digits, found " + describe(c));
            v = (v << 4) | d;
        }
        return (char) v;
    }

    /**
     * The shared renderer, which is the whole point of handing it these types rather
     * than rendering here. The checked exception belongs to the CLOB branch, which a
     * JSON value can never reach.
     */
    private String render(Object v, int column) {
        try {
            return SqlValues.render(v, column + 1);
        } catch (SQLException e) {
            throw new CsvDiffException("could not render the value of column "
                    + header.names().get(column) + " in row " + row, e);
        }
    }

    /* ------------------------------------------------------------ the chars -- */

    private void literal(String word) {
        for (int i = 0; i < word.length(); i++) {
            int c = take();
            if (c != word.charAt(i)) {
                throw error("expected " + word + " in row " + row + ", found " + describe(c));
            }
        }
    }

    private void expect(char c, String what) {
        int got = take();
        if (got != c) throw error("expected " + c + ", found " + describe(got) + ": " + what);
    }

    private void skipWhitespace() {
        for (;;) {
            int c = peek();
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
            take();
        }
    }

    private int peek() {
        if (at >= limit && !fill()) return -1;
        return buf[at];
    }

    private int take() {
        if (at >= limit && !fill()) return -1;
        char c = buf[at++];
        offset++;
        if (c == '\n') {
            line++;
            lineStart = offset;
        }
        return c;
    }

    private boolean fill() {
        if (eof) return false;
        try {
            int n = in.read(buf);
            if (n < 0) {
                eof = true;
                at = 0;
                limit = 0;
                return false;
            }
            at = 0;
            limit = n;
            return n > 0 || fill();
        } catch (IOException e) {
            throw new CsvDiffException("reading the JSON failed: " + e.getMessage(), e);
        }
    }

    private static String describe(int c) {
        if (c == -1) return "the end of the input";
        if (c == '\n') return "a newline";
        return "'" + (char) c + "'";
    }

    private CsvDiffException error(String message) {
        return new CsvDiffException(message + " (line " + line + ", column "
                + (offset - lineStart) + ")");
    }
}
