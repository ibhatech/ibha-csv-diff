package com.ibhatech.csvdiff;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A minimal JSON reader, for exactly one job: parsing what the summary emitter
 * wrote.
 *
 * <p>Why this exists rather than a dependency. The summary a consumer gets from
 * this binding and the summary the emitter writes to a file must not drift, and the
 * only way to guarantee that is to read the emitter's own bytes rather than to
 * assemble a parallel object from the stats struct. That needs a JSON reader. A
 * library that pulls in Jackson to read one object it produced itself imposes a
 * transitive dependency, and its version conflicts, on every consuming
 * application, which for a binding whose entire purpose is to be embedded in
 * someone else's server is a poor trade against a hundred and fifty lines.
 *
 * <p>It is deliberately not a general JSON library and is not public. It reads what
 * the emitter emits: objects, arrays, strings with the escapes the emitter writes,
 * numbers, the three literals. Integral numbers come back as {@link Long} and the
 * rest as {@link Double}.
 */
final class Json {

    private final String src;
    private int at;

    private Json(String src) {
        this.src = src;
    }

    /** Parses one complete JSON value. */
    static Object parse(String text) {
        Json p = new Json(text);
        p.ws();
        Object v = p.value();
        p.ws();
        if (p.at != p.src.length()) {
            throw new CsvDiffException("trailing text in the engine's JSON at offset " + p.at);
        }
        return v;
    }

    private Object value() {
        if (at >= src.length()) throw fail("a value");
        char c = src.charAt(at);
        return switch (c) {
            case '{' -> object();
            case '[' -> array();
            case '"' -> string();
            case 't' -> literal("true", Boolean.TRUE);
            case 'f' -> literal("false", Boolean.FALSE);
            case 'n' -> literal("null", null);
            default -> number();
        };
    }

    private Map<String, Object> object() {
        Map<String, Object> out = new LinkedHashMap<>();
        expect('{');
        ws();
        if (peek() == '}') {
            at++;
            return out;
        }
        for (;;) {
            ws();
            String k = string();
            ws();
            expect(':');
            ws();
            out.put(k, value());
            ws();
            char c = next();
            if (c == '}') return out;
            if (c != ',') throw fail("',' or '}'");
        }
    }

    private List<Object> array() {
        List<Object> out = new ArrayList<>();
        expect('[');
        ws();
        if (peek() == ']') {
            at++;
            return out;
        }
        for (;;) {
            ws();
            out.add(value());
            ws();
            char c = next();
            if (c == ']') return out;
            if (c != ',') throw fail("',' or ']'");
        }
    }

    private String string() {
        expect('"');
        StringBuilder sb = new StringBuilder();
        for (;;) {
            char c = next();
            if (c == '"') return sb.toString();
            if (c != '\\') {
                sb.append(c);
                continue;
            }
            char e = next();
            switch (e) {
                case '"', '\\', '/' -> sb.append(e);
                case 'b' -> sb.append('\b');
                case 'f' -> sb.append('\f');
                case 'n' -> sb.append('\n');
                case 'r' -> sb.append('\r');
                case 't' -> sb.append('\t');
                case 'u' -> {
                    // The emitter escapes only the control characters this way and
                    // leaves everything else as UTF-8, so this is always a
                    // four digit BMP escape and never a surrogate pair to
                    // reassemble. (Written without the backslash-u spelling on
                    // purpose: Java expands unicode escapes inside comments too,
                    // which makes writing one here a compile error.)
                    sb.append((char) Integer.parseInt(src.substring(at, at + 4), 16));
                    at += 4;
                }
                default -> throw fail("a valid escape, found \\" + e);
            }
        }
    }

    private Object number() {
        int start = at;
        if (peek() == '-') at++;
        boolean integral = true;
        while (at < src.length()) {
            char c = src.charAt(at);
            if (c >= '0' && c <= '9') {
                at++;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                integral = false;
                at++;
            } else {
                break;
            }
        }
        String text = src.substring(start, at);
        if (text.isEmpty()) throw fail("a number");
        return integral ? (Object) Long.valueOf(text) : (Object) Double.valueOf(text);
    }

    private Object literal(String word, Object v) {
        if (!src.startsWith(word, at)) throw fail(word);
        at += word.length();
        return v;
    }

    private void ws() {
        while (at < src.length()) {
            char c = src.charAt(at);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') at++;
            else break;
        }
    }

    private char peek() {
        return at < src.length() ? src.charAt(at) : '\0';
    }

    private char next() {
        if (at >= src.length()) throw fail("more input");
        return src.charAt(at++);
    }

    private void expect(char c) {
        if (next() != c) throw fail("'" + c + "'");
    }

    private CsvDiffException fail(String wanted) {
        return new CsvDiffException(
                "the engine's JSON is malformed at offset " + at + ": expected " + wanted);
    }

    /* ------------------------------------------------------------ accessors -- */

    @SuppressWarnings("unchecked")
    static Map<String, Object> obj(Object o, String key) {
        if (!(o instanceof Map<?, ?> m)) return Map.of();
        Object v = m.get(key);
        return v instanceof Map<?, ?> ? (Map<String, Object>) v : Map.of();
    }

    @SuppressWarnings("unchecked")
    static List<Object> arr(Object o, String key) {
        if (!(o instanceof Map<?, ?> m)) return List.of();
        Object v = m.get(key);
        return v instanceof List<?> ? (List<Object>) v : List.of();
    }

    static long num(Object o, String key) {
        if (!(o instanceof Map<?, ?> m)) return 0;
        Object v = m.get(key);
        if (v instanceof Long l) return l;
        if (v instanceof Double d) return d.longValue();
        return 0;
    }

    static boolean bool(Object o, String key) {
        return o instanceof Map<?, ?> m && Boolean.TRUE.equals(m.get(key));
    }

    static String str(Object o, String key) {
        if (!(o instanceof Map<?, ?> m)) return "";
        Object v = m.get(key);
        return v instanceof String s ? s : "";
    }

    static boolean has(Object o, String key) {
        return o instanceof Map<?, ?> m && m.containsKey(key);
    }
}
