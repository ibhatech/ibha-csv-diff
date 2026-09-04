/*
 * emitkit.h - independent checkers for what the emitters produce.
 *
 * These deliberately do not share a line of code with src/emit.c. An emitter
 * asserting that its own output is what it meant to write proves nothing; the
 * value is in a second implementation, written from the format's rules rather
 * than from the emitter's, disagreeing with it.
 *
 * Two checkers, and both are security properties rather than formatting ones.
 *
 * `ek_jsonl_ok` is a strict JSON parser. It rejects an unescaped quote, an
 * unescaped control byte and ill formed UTF-8, which are exactly the three ways a
 * cell value can break out of a JSON string and turn one report row into two.
 *
 * `ek_html_ok` is the XSS invariant stated positively: **every `<` in the output
 * opens one of a fixed list of tags this emitter is allowed to write, and every
 * `&` opens one of five entities.** Anything else means cell content reached the
 * document as markup. A test that merely greps for "<script>" would pass on
 * "<img src=x onerror=alert(1)>"; this cannot.
 */
#ifndef IBHA_TEST_EMITKIT_H
#define IBHA_TEST_EMITKIT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ json -- */

static const char *ek_json_value(const char *p, const char *e, int depth);

static const char *ek_ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static int ek_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static const char *ek_json_string(const char *p, const char *e) {
    if (p >= e || *p != '"') return NULL;
    p++;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20u) return NULL; /* a raw control byte ends the string early */
        if (c == '\\') {
            p++;
            if (p >= e) return NULL;
            switch (*p) {
                case '"': case '\\': case '/': case 'b':
                case 'f': case 'n': case 'r': case 't': p++; break;
                case 'u':
                    if (e - p < 5) return NULL;
                    for (int i = 1; i <= 4; i++) {
                        if (!ek_hex(p[i])) return NULL;
                    }
                    p += 5;
                    break;
                default: return NULL;
            }
            continue;
        }
        p++;
    }
    return NULL;
}

static const char *ek_json_number(const char *p, const char *e) {
    const char *s = p;
    if (p < e && *p == '-') p++;
    if (p >= e) return NULL;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < e && *p >= '0' && *p <= '9') p++;
    } else {
        return NULL;
    }
    if (p < e && *p == '.') {
        p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || *p < '0' || *p > '9') return NULL;
        while (p < e && *p >= '0' && *p <= '9') p++;
    }
    return p > s ? p : NULL;
}

static const char *ek_json_lit(const char *p, const char *e, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(e - p) < n || memcmp(p, lit, n) != 0) return NULL;
    return p + n;
}

static const char *ek_json_value(const char *p, const char *e, int depth) {
    if (depth > 32) return NULL;
    p = ek_ws(p, e);
    if (p >= e) return NULL;

    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = open == '{' ? '}' : ']';
        p = ek_ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (open == '{') {
                p = ek_json_string(ek_ws(p, e), e);
                if (!p) return NULL;
                p = ek_ws(p, e);
                if (p >= e || *p != ':') return NULL;
                p++;
            }
            p = ek_json_value(p, e, depth + 1);
            if (!p) return NULL;
            p = ek_ws(p, e);
            if (p < e && *p == ',') {
                p++;
                continue;
            }
            if (p < e && *p == close) return p + 1;
            return NULL;
        }
    }
    if (*p == '"') return ek_json_string(p, e);
    if (*p == 't') return ek_json_lit(p, e, "true");
    if (*p == 'f') return ek_json_lit(p, e, "false");
    if (*p == 'n') return ek_json_lit(p, e, "null");
    return ek_json_number(p, e);
}

/* Well formed UTF-8, strictly: no overlong forms, no surrogates, nothing above
 * U+10FFFF. A JSON document that is not valid UTF-8 is not a JSON document. */
static int ek_utf8_ok(const char *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        size_t n;
        if (c < 0x80u) {
            i++;
            continue;
        }
        if (c >= 0xC2u && c <= 0xDFu) {
            n = 2;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            n = 3;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            n = 4;
        } else {
            return 0;
        }
        if (i + n > len) return 0;
        for (size_t k = 1; k < n; k++) {
            if ((p[i + k] & 0xC0u) != 0x80u) return 0;
        }
        if (n == 3 && c == 0xE0u && p[i + 1] < 0xA0u) return 0;
        if (n == 3 && c == 0xEDu && p[i + 1] > 0x9Fu) return 0;
        if (n == 4 && c == 0xF0u && p[i + 1] < 0x90u) return 0;
        if (n == 4 && c == 0xF4u && p[i + 1] > 0x8Fu) return 0;
        i += n;
    }
    return 1;
}

/* Every line is one complete JSON value and nothing else. Returns the line count,
 * or -1 on the first line that is not. */
static int ek_jsonl_ok(const char *buf, size_t len) {
    size_t at = 0;
    int lines = 0;
    if (!ek_utf8_ok(buf, len)) return -1;

    while (at < len) {
        const char *line = buf + at;
        const char *nl = (const char *)memchr(line, '\n', len - at);
        if (!nl) return -1; /* every row is terminated, so a partial line is a bug */
        size_t n = (size_t)(nl - line);
        const char *end = ek_json_value(line, line + n, 0);
        if (!end || ek_ws(end, line + n) != line + n) return -1;
        lines++;
        at += n + 1;
    }
    return lines;
}

/* ------------------------------------------------------------------ html -- */

/* Everything the emitter is allowed to write. A '<' that opens anything else is
 * cell content that became markup. */
static const char *const ek_tags[] = {
    "<div ",   "<div>",    "<table ", "<table>",  "<thead>",  "<tbody>",  "<tr ",
    "<tr>",    "<th ",     "<th>",    "<td ",     "<td>",     "<span ",   "<span>",
    "<del ",   "<del>",    "<ins ",   "<ins>",    "</div>",   "</table>", "</thead>",
    "</tbody>", "</tr>",   "</th>",   "</td>",    "</span>",  "</del>",   "</ins>",
};

static const char *const ek_entities[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&#39;"};

/*
 * Returns 1 when every '<' opens a known tag, every '&' opens a known entity, and
 * no attribute value contains a raw quote. Written from the rules of HTML rather
 * than from the emitter's structure, on purpose.
 */
static int ek_html_ok(const char *buf, size_t len) {
    if (!ek_utf8_ok(buf, len)) return 0;

    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '&') {
            int ok = 0;
            for (size_t k = 0; k < sizeof(ek_entities) / sizeof(ek_entities[0]); k++) {
                size_t n = strlen(ek_entities[k]);
                if (len - i >= n && memcmp(buf + i, ek_entities[k], n) == 0) ok = 1;
            }
            if (!ok) return 0;
            continue;
        }
        if (buf[i] != '<') continue;

        size_t tag = 0;
        int matched = 0;
        for (size_t k = 0; k < sizeof(ek_tags) / sizeof(ek_tags[0]); k++) {
            size_t n = strlen(ek_tags[k]);
            if (len - i >= n && memcmp(buf + i, ek_tags[k], n) == 0) {
                matched = 1;
                tag = n;
                break;
            }
        }
        if (!matched) return 0;
        if (buf[i + tag - 1] == '>') {
            /* A tag that carries no attributes is already complete. */
            i += tag - 1;
            continue;
        }

        /* Inside an open tag, attribute values are the only place text appears,
         * and each must be a quoted run with no '<' and no stray quote in it. */
        size_t j = i + tag;
        while (j < len && buf[j] != '>') {
            if (buf[j] == '"') {
                j++;
                while (j < len && buf[j] != '"') {
                    if (buf[j] == '<' || buf[j] == '&') return 0;
                    j++;
                }
                if (j >= len) return 0;
            } else if (buf[j] == '<') {
                return 0;
            }
            j++;
        }
        if (j >= len) return 0;
        i = j;
    }
    return 1;
}

/* Whether the needle appears literally, which is how the XSS assertions say
 * "this payload did not survive as markup". */
static int ek_contains(const char *buf, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (n > len) return 0;
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(buf + i, needle, n) == 0) return 1;
    }
    return 0;
}

#endif /* IBHA_TEST_EMITKIT_H */
