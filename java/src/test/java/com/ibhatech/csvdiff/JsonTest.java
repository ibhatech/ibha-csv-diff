package com.ibhatech.csvdiff;

import org.junit.jupiter.api.Test;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * The minimal JSON reader.
 *
 * <p>It only ever reads what this project's own emitters write, so these tests are
 * about the shapes those produce: nested objects, arrays of objects, escaped
 * strings, negative and large integers, and the three literals.
 */
class JsonTest {

    @Test
    void readsTheShapesTheEmittersWrite() {
        Object o = Json.parse("""
                {"schemaVersion":1,"identical":false,"rows":{"unchanged":10,"moved":0},
                 "schemaFindings":[{"kind":"columnAdded","column":3,"name":"extra"}],
                 "nothing":null}
                """);
        assertEquals(1, Json.num(o, "schemaVersion"));
        assertEquals(false, Json.bool(o, "identical"));
        assertEquals(10, Json.num(Json.obj(o, "rows"), "unchanged"));
        assertEquals(1, Json.arr(o, "schemaFindings").size());
        assertEquals("columnAdded", Json.str(Json.arr(o, "schemaFindings").get(0), "kind"));
        assertTrue(Json.has(o, "nothing"));
    }

    @Test
    void readsTheEscapesTheEmitterWrites() {
        Object o = Json.parse("{\"a\":\"q\\\"uote\",\"b\":\"tab\\there\",\"c\":\"nl\\nhere\","
                + "\"d\":\"\\u0001\",\"e\":\"back\\\\slash\"}");
        assertEquals("q\"uote", Json.str(o, "a"));
        assertEquals("tab\there", Json.str(o, "b"));
        assertEquals("nl\nhere", Json.str(o, "c"));
        assertEquals(1, Json.str(o, "d").charAt(0));
        assertEquals("back\\slash", Json.str(o, "e"));
    }

    @Test
    void keepsNonAsciiIntact() {
        String text = new String("{\"a\":\"café ✅ 𝄞\"}".getBytes(StandardCharsets.UTF_8),
                StandardCharsets.UTF_8);
        assertEquals("café ✅ 𝄞", Json.str(Json.parse(text), "a"));
    }

    @Test
    void handlesNegativeAndLargeNumbers() {
        Object o = Json.parse("{\"a\":-42,\"b\":9007199254740993,\"c\":1.5}");
        assertEquals(-42, Json.num(o, "a"));
        assertEquals(9007199254740993L, Json.num(o, "b"), "must not go through a double");
        assertEquals(1, Json.num(o, "c"));
    }

    @Test
    void handlesEmptyContainers() {
        Object o = Json.parse("{\"a\":[],\"b\":{}}");
        assertEquals(List.of(), Json.arr(o, "a"));
        assertEquals(Map.of(), Json.obj(o, "b"));
    }

    @Test
    void refusesMalformedInputRatherThanGuessing() {
        assertThrows(CsvDiffException.class, () -> Json.parse("{\"a\":}"));
        assertThrows(CsvDiffException.class, () -> Json.parse("{\"a\":1"));
        assertThrows(CsvDiffException.class, () -> Json.parse("{\"a\":1} trailing"));
        assertThrows(CsvDiffException.class, () -> Json.parse(""));
    }

    /** A missing key reads as a zero or an empty rather than throwing, because the
     *  summary gains fields over time and an older reader should degrade rather
     *  than refuse to open a newer report. */
    @Test
    void missingKeysAreAbsentNotFatal() {
        Object o = Json.parse("{}");
        assertEquals(0, Json.num(o, "nope"));
        assertEquals("", Json.str(o, "nope"));
        assertEquals(false, Json.bool(o, "nope"));
        assertEquals(List.of(), Json.arr(o, "nope"));
    }
}
