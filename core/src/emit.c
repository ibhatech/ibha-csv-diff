/*
 * emit.c - the emitters of spec 13.3.
 *
 * The library must not decide the destination and must never require the whole
 * diff in memory to reach one. So there is exactly one output primitive, the pull
 * cursor, and everything in this file is a loop over it writing into a caller
 * supplied sink. The emitter's own memory is one 8 KB output buffer and one
 * report row, whatever the size of the diff.
 *
 * Three things in here are contracts rather than implementation details.
 *
 * **The row shape is versioned.** The emitter and the consumer are separate
 * components that have to agree, so IBHA_CSVD_SCHEMA_VERSION rides on every JSONL
 * row, on every CSV row and on the HTML container, and changing a shape is a
 * breaking change.
 *
 * **Escaping is a security requirement, not a formatting one.** Cell content
 * arrives from a salesman's spreadsheet and the output is injected into a page
 * through dangerouslySetInnerHTML or written to a file someone opens in a
 * browser. A cell containing <script> or " onload=" must not become live markup.
 * Getting this wrong turns a diff preview into stored XSS against the approver,
 * so it is arranged to be correct by construction rather than by remembering:
 *
 *   - every value goes through wr_value, which escapes for the format in hand;
 *   - the only things written outside it are compiled in literals and integers;
 *   - class names come from a fixed compiled in set, and the one caller supplied
 *     string that reaches the markup, the class prefix, is validated against
 *     [A-Za-z][A-Za-z0-9_-]{0,31} and refused otherwise;
 *   - no caller data ever reaches an attribute name or a URL, and no URL is
 *     emitted at all.
 *
 * **Invalid UTF-8 is replaced, not passed through, in JSON and HTML.** For JSON
 * that is because a JSONL line has to parse. For HTML it is a second XSS defence:
 * an overlong encoding of '<' is the classic filter bypass, it is not the byte
 * 0x3C so a byte oriented escaper passes it through untouched, and a decoder that
 * accepts overlongs then sees a tag. Rejecting every ill formed sequence closes
 * that off. CSV has no encoding contract, carries no markup and is byte
 * transparent, so it passes bytes through unchanged.
 */
#include "internal.h"

#define EMIT_BUF 8192u

/* Longest run of schema findings the summary reports before giving up on
 * enumerating them. The count is still exact; only the list is bounded, because
 * the summary is defined as constant memory and a wrong header row would
 * otherwise produce one finding per column. */
#define EMIT_MAX_SCHEMA_FINDINGS 64u

/* Segments considered when the HTML emitter is highlighting inside a cell. A
 * cell needing more than this reports as wholly replaced, which is what the
 * segment API itself does past its own cap. */
#define EMIT_MAX_SEGMENTS 128u

static const char *const k_kind[4] = {"unchanged", "modified", "added", "deleted"};

/* ------------------------------------------------------------------ sink -- */

typedef struct {
    const ibha_csvd_sink *sink;
    size_t len;
    int err;
    uint8_t buf[EMIT_BUF];
} emit_w;

static void wr_flush(emit_w *w) {
    if (w->err || w->len == 0) return;
    if (w->sink->write(w->sink->ctx, w->buf, w->len) < 0) w->err = 1;
    w->len = 0;
}

static void wr(emit_w *w, const void *bytes, size_t n) {
    const uint8_t *p = (const uint8_t *)bytes;
    while (n && !w->err) {
        size_t room = EMIT_BUF - w->len;
        size_t take = n < room ? n : room;
        IBHA_MEMCPY(w->buf + w->len, p, take);
        w->len += take;
        p += take;
        n -= take;
        if (w->len == EMIT_BUF) wr_flush(w);
    }
}

static void wr_ch(emit_w *w, char c) {
    if (w->err) return;
    w->buf[w->len++] = (uint8_t)c;
    if (w->len == EMIT_BUF) wr_flush(w);
}

static void wr_str(emit_w *w, const char *s) { wr(w, s, IBHA_STRLEN(s)); }

static void wr_u64(emit_w *w, uint64_t v) {
    char tmp[20];
    int n = 0;
    if (v == 0) {
        wr_ch(w, '0');
        return;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) wr_ch(w, tmp[--n]);
}

static void wr_i32(emit_w *w, int32_t v) {
    if (v < 0) {
        wr_ch(w, '-');
        wr_u64(w, (uint64_t)0 - (uint64_t)v);
    } else {
        wr_u64(w, (uint64_t)v);
    }
}

static void wr_bool(emit_w *w, int v) { wr_str(w, v ? "true" : "false"); }

/* ---------------------------------------------------------------- values -- */

#define ESC_JSON 0
#define ESC_HTML 1
#define ESC_CSV 2

#define VAL_TRUNCATED 0x01u
#define VAL_INVALID_UTF8 0x02u

/* Walks a field's logical value: the raw range with every "" collapsed to ".
 * Nothing is materialized; this is a pointer walk over the parsed bytes. */
typedef struct {
    const uint8_t *p;
    uint32_t len;
    uint32_t i;
    int esc;
    uint8_t quote;
    int pending; /* one byte of pushback, or -1 */
} fiter;

static void fi_open(fiter *it, const ibha_csvd_table *t, uint32_t f) {
    it->p = t->bytes + t->field_off[f];
    it->len = t->field_len[f];
    it->i = 0;
    it->esc = t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE;
    it->quote = t->quote;
    it->pending = -1;
}

static int fi_next(fiter *it) {
    if (it->pending >= 0) {
        int c = it->pending;
        it->pending = -1;
        return c;
    }
    if (it->i >= it->len) return -1;
    uint8_t c = it->p[it->i];
    if (it->esc && c == it->quote && it->i + 1 < it->len && it->p[it->i + 1] == it->quote) it->i++;
    it->i++;
    return (int)c;
}

static void fi_push(fiter *it, int c) { it->pending = c; }

/* How many bytes the UTF-8 sequence opened by c occupies, or 0 when c cannot
 * open one. Strict per RFC 3629: no overlong forms, no surrogates, nothing above
 * U+10FFFF. The narrow lead byte ranges are what reject the overlong encodings
 * an escaper working a byte at a time would otherwise pass straight through. */
static uint32_t utf8_len(uint8_t c) {
    if (c < 0x80u) return 1;
    if (c >= 0xC2u && c <= 0xDFu) return 2;
    if (c >= 0xE0u && c <= 0xEFu) return 3;
    if (c >= 0xF0u && c <= 0xF4u) return 4;
    return 0;
}

static int utf8_tail_ok(uint8_t lead, const uint8_t *seq, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        if ((seq[i] & 0xC0u) != 0x80u) return 0;
    }
    if (n == 3) {
        if (lead == 0xE0u && seq[1] < 0xA0u) return 0; /* overlong */
        if (lead == 0xEDu && seq[1] > 0x9Fu) return 0; /* surrogate */
    }
    if (n == 4) {
        if (lead == 0xF0u && seq[1] < 0x90u) return 0;  /* overlong */
        if (lead == 0xF4u && seq[1] > 0x8Fu) return 0;  /* above U+10FFFF */
    }
    return 1;
}

static void wr_escaped_byte(emit_w *w, uint8_t c, int mode) {
    switch (mode) {
        case ESC_JSON:
            switch (c) {
                case '"': wr_str(w, "\\\""); return;
                case '\\': wr_str(w, "\\\\"); return;
                case '\n': wr_str(w, "\\n"); return;
                case '\r': wr_str(w, "\\r"); return;
                case '\t': wr_str(w, "\\t"); return;
                case '\b': wr_str(w, "\\b"); return;
                case '\f': wr_str(w, "\\f"); return;
                default: break;
            }
            if (c < 0x20u) {
                static const char k_hex[] = "0123456789abcdef";
                wr_str(w, "\\u00");
                wr_ch(w, k_hex[(c >> 4) & 0x0Fu]);
                wr_ch(w, k_hex[c & 0x0Fu]);
                return;
            }
            wr_ch(w, (char)c);
            return;

        case ESC_HTML:
            /* All five, not just the three that close a tag: a value landing in
             * an attribute is one refactor away, and " onload=" is exactly the
             * payload the XSS fixture carries. */
            switch (c) {
                case '&': wr_str(w, "&amp;"); return;
                case '<': wr_str(w, "&lt;"); return;
                case '>': wr_str(w, "&gt;"); return;
                case '"': wr_str(w, "&quot;"); return;
                case '\'': wr_str(w, "&#39;"); return;
                case 0: wr_str(w, "\xEF\xBF\xBD"); return; /* NUL is not markup */
                default: wr_ch(w, (char)c); return;
            }

        default: /* ESC_CSV: the field is quoted when it needs to be, and a quote
                  * inside it is doubled. Bytes are otherwise untouched. */
            if (c == '"') {
                wr_str(w, "\"\"");
            } else {
                wr_ch(w, (char)c);
            }
            return;
    }
}

/*
 * Writes a field's logical value, escaped for the format in hand, optionally
 * skipping the first `skip` logical bytes and taking at most `take` of them.
 * take of 0 means the rest of the value.
 *
 * Truncation stops on a whole UTF-8 sequence, so a capped value is still valid
 * text rather than a value ending in half a character.
 */
static void wr_value(emit_w *w, const ibha_csvd_table *t, uint32_t f, int mode, uint32_t skip,
                     uint32_t take, uint32_t cap, uint32_t *flags) {
    fiter it;
    uint8_t seq[4];
    uint32_t written = 0, seen = 0;

    fi_open(&it, t, f);
    for (;;) {
        int c = fi_next(&it);
        if (c < 0) break;

        seq[0] = (uint8_t)c;
        uint32_t n = utf8_len(seq[0]);
        uint32_t got = 1;
        while (n > 1 && got < n) {
            int k = fi_next(&it);
            if (k < 0) break;
            /* A byte that cannot continue this sequence belongs to the next one:
             * a lone 0xE9 in a Latin-1 file must cost one replacement character,
             * not swallow the two bytes after it. */
            if ((k & 0xC0) != 0x80) {
                fi_push(&it, k);
                break;
            }
            seq[got++] = (uint8_t)k;
        }
        int valid = n > 0 && got == n && utf8_tail_ok(seq[0], seq, n);

        /* Skipping is expressed in logical bytes, which is the unit the segment
         * offsets use, and it advances a whole character at a time: an offset
         * landing inside a sequence would otherwise emit its tail bytes alone. */
        if (seen < skip) {
            seen += got;
            continue;
        }
        if (take && written + got > take) break;
        if (cap && written + got > cap) {
            if (flags) *flags |= VAL_TRUNCATED;
            break;
        }

        if (!valid) {
            if (flags) *flags |= VAL_INVALID_UTF8;
            if (mode == ESC_CSV) {
                for (uint32_t i = 0; i < got; i++) wr_escaped_byte(w, seq[i], mode);
            } else if (mode == ESC_JSON) {
                wr_str(w, "\\ufffd");
            } else {
                wr_str(w, "\xEF\xBF\xBD");
            }
            written += got;
            continue;
        }

        if (n == 1) {
            wr_escaped_byte(w, seq[0], mode);
        } else {
            wr(w, seq, n); /* a well formed non ASCII sequence is never markup */
        }
        written += got;
    }
}

/* ------------------------------------------------------------ the values -- */

typedef struct {
    ibha_csvd_diff *d;
    const ibha_csvd_emit_opts *o;
    emit_w w;
    const char *prefix;   /* validated HTML class prefix */
    uint32_t name_row;    /* source table row holding the column names, or NO_ROW */
    uint32_t rows_written;
} emitter;

/* The column name, from the source file, which spec 13.8 makes authoritative.
 * It is file content and therefore untrusted, so it goes through wr_value like
 * any other value. */
static void wr_col_name(emitter *e, uint32_t col, int mode) {
    if (e->name_row != IBHA_CSVD_NO_ROW) {
        uint32_t f = ibha_csvd_row_field(e->d->src, e->name_row, col);
        if (f != 0xFFFFFFFFu) {
            wr_value(&e->w, e->d->src, f, mode, 0, 0, 0, NULL);
            return;
        }
    }
    wr_str(&e->w, "column_");
    wr_u64(&e->w, col + 1);
}

/* The field of one column on one side of a report row, or UINT32_MAX. */
static uint32_t row_field(const ibha_csvd_table *t, uint32_t row, uint32_t col) {
    if (row == IBHA_CSVD_NO_ROW) return 0xFFFFFFFFu;
    return ibha_csvd_row_field(t, row, col);
}

/* 1 based record number, counting header records, which is what every message in
 * the engine already means by a row number. */
static uint64_t row_number(uint32_t row) { return (uint64_t)row + 1u; }

/* ------------------------------------------------------------------ jsonl -- */

static const char *finding_name(uint8_t flag) {
    switch (flag) {
        case IBHA_CSVD_CELL_REQUIRED_EMPTY: return "requiredEmpty";
        case IBHA_CSVD_CELL_TOO_LONG: return "tooLong";
        case IBHA_CSVD_CELL_NOT_NUMERIC: return "notNumeric";
        default: return "precision";
    }
}

static void jsonl_findings(emitter *e, const ibha_csvd_row *r) {
    static const uint8_t k_flags[4] = {IBHA_CSVD_CELL_REQUIRED_EMPTY, IBHA_CSVD_CELL_TOO_LONG,
                                       IBHA_CSVD_CELL_NOT_NUMERIC, IBHA_CSVD_CELL_PRECISION};
    const ibha_csvd_schema *s = e->d->ss;
    int first = 1;

    wr_str(&e->w, ",\"findings\":[");
    for (uint32_t c = 0; c < r->n_columns; c++) {
        for (uint32_t k = 0; k < 4; k++) {
            if (!(r->cell_flags[c] & k_flags[k])) continue;
            if (!first) wr_ch(&e->w, ',');
            first = 0;
            wr_str(&e->w, "{\"column\":");
            wr_u64(&e->w, c);
            wr_str(&e->w, ",\"name\":\"");
            wr_col_name(e, c, ESC_JSON);
            wr_str(&e->w, "\",\"kind\":\"");
            wr_str(&e->w, finding_name(k_flags[k]));
            wr_ch(&e->w, '"');
            if (k_flags[k] == IBHA_CSVD_CELL_TOO_LONG && s->col_size) {
                wr_str(&e->w, ",\"limit\":");
                wr_i32(&e->w, s->col_size[c]);
            }
            if (k_flags[k] == IBHA_CSVD_CELL_PRECISION && s->col_size) {
                wr_str(&e->w, ",\"precision\":");
                wr_i32(&e->w, s->col_size[c]);
                wr_str(&e->w, ",\"scale\":");
                wr_i32(&e->w, s->col_scale ? s->col_scale[c] : -1);
            }
            wr_ch(&e->w, '}');
        }
    }
    wr_ch(&e->w, ']');
}

static void emit_jsonl_row(emitter *e, const ibha_csvd_row *r) {
    emit_w *w = &e->w;

    wr_str(w, "{\"schemaVersion\":");
    wr_u64(w, (uint64_t)IBHA_CSVD_SCHEMA_VERSION);
    wr_str(w, ",\"kind\":\"");
    wr_str(w, k_kind[r->kind & 3u]);
    wr_str(w, "\",\"sourceRow\":");
    if (r->source_row == IBHA_CSVD_NO_ROW) {
        wr_str(w, "null");
    } else {
        wr_u64(w, row_number(r->source_row));
    }
    wr_str(w, ",\"targetRow\":");
    if (r->target_row == IBHA_CSVD_NO_ROW) {
        wr_str(w, "null");
    } else {
        wr_u64(w, row_number(r->target_row));
    }
    wr_str(w, ",\"moved\":");
    wr_bool(w, r->moved);
    wr_str(w, ",\"moveDistance\":");
    wr_i32(w, r->move_distance);

    if (e->o->include_values) {
        wr_str(w, ",\"cells\":[");
        for (uint32_t c = 0; c < r->n_columns; c++) {
            uint32_t fs = row_field(e->d->src, r->source_row, c);
            uint32_t ft = row_field(e->d->tgt, r->target_row, c);
            uint8_t fl = r->cell_flags[c];
            uint32_t vflags = 0;

            if (c) wr_ch(w, ',');
            wr_str(w, "{\"name\":\"");
            wr_col_name(e, c, ESC_JSON);
            wr_ch(w, '"');

            /*
             * The contract, stated once: a matched row carries "source" exactly
             * when the cell differs in bytes from the target. Its absence means
             * the two sides are byte identical, which is what keeps an unchanged
             * 90,000 row report from being written twice over.
             */
            int want_source = fs != 0xFFFFFFFFu &&
                              (ft == 0xFFFFFFFFu ||
                               (fl & (IBHA_CSVD_CELL_CHANGED | IBHA_CSVD_CELL_SUPPRESSED)) != 0);
            if (want_source) {
                wr_str(w, ",\"source\":\"");
                wr_value(w, e->d->src, fs, ESC_JSON, 0, 0, e->o->max_cell_bytes, &vflags);
                wr_ch(w, '"');
            }
            if (ft != 0xFFFFFFFFu) {
                wr_str(w, ",\"target\":\"");
                wr_value(w, e->d->tgt, ft, ESC_JSON, 0, 0, e->o->max_cell_bytes, &vflags);
                wr_ch(w, '"');
            }
            if (fl & IBHA_CSVD_CELL_CHANGED) wr_str(w, ",\"changed\":true");
            if (fl & IBHA_CSVD_CELL_SUPPRESSED) wr_str(w, ",\"suppressed\":true");
            if (vflags & VAL_TRUNCATED) wr_str(w, ",\"truncated\":true");
            if (vflags & VAL_INVALID_UTF8) wr_str(w, ",\"invalidUtf8\":true");
            wr_ch(w, '}');
        }
        wr_ch(w, ']');
    }

    if (r->n_findings) jsonl_findings(e, r);
    wr_str(w, "}\n");
}

/* -------------------------------------------------------------------- csv -- */

/*
 * Whether a value needs quoting, and whether it needs the formula guard. One
 * walk answers both, because both questions are about the same bytes.
 *
 * The guard: a value opening with '=', '+', '@', a tab or a CR is a formula to
 * Excel, so a diff report of untrusted data is a script delivery mechanism unless
 * something intervenes. A leading '-' is guarded only when what follows is not a
 * plain number, because prefixing every negative amount in a financial report
 * would be worse than the risk.
 */
static void csv_scan(const ibha_csvd_table *t, uint32_t f, uint8_t delim, int *need_quote,
                     int *need_guard) {
    fiter it;
    int first = -1, second = -1;
    int dots = 0, other = 0, n = 0;

    *need_quote = 0;
    *need_guard = 0;
    fi_open(&it, t, f);
    for (;;) {
        int c = fi_next(&it);
        if (c < 0) break;
        if (n == 0) first = c;
        if (n == 1) second = c;
        n++;
        if (c == delim || c == '"' || c == '\n' || c == '\r') *need_quote = 1;
        /* Everything after the first byte, which is the sign when there is one:
         * a plain number has digits, at most one point, and nothing else. */
        if (n > 1 && !(c >= '0' && c <= '9')) {
            if (c == '.') {
                dots++;
            } else {
                other++;
            }
        }
    }
    if (n == 0) return;
    if (first == ' ' || first == '\t') *need_quote = 1;

    if (first == '=' || first == '+' || first == '@' || first == '\t' || first == '\r') {
        *need_guard = 1;
    } else if (first == '-') {
        int plain_number = (second >= '0' && second <= '9') && other == 0 && dots <= 1;
        *need_guard = !plain_number;
    }
    if (*need_guard) *need_quote = 1;
}

static void csv_field(emitter *e, const ibha_csvd_table *t, uint32_t f) {
    int need_quote = 0, need_guard = 0;
    if (f == 0xFFFFFFFFu) return;

    csv_scan(t, f, e->o->csv_delimiter, &need_quote, &need_guard);
    if (!e->o->csv_formula_guard) need_guard = 0;

    if (need_quote) wr_ch(&e->w, '"');
    if (need_guard) wr_ch(&e->w, '\'');
    wr_value(&e->w, t, f, ESC_CSV, 0, 0, e->o->max_cell_bytes, NULL);
    if (need_quote) wr_ch(&e->w, '"');
}

static void csv_text(emitter *e, const char *s) {
    /* Compiled in text only: every one of these is an identifier from a fixed
     * set, so it needs no quoting and gets none. */
    wr_str(&e->w, s);
}

static void emit_csv_header(emitter *e) {
    static const char *const k_meta[9] = {"schemaVersion", "kind",      "side",
                                          "moved",         "moveDistance", "sourceRow",
                                          "targetRow",     "changedCells", "findings"};
    emit_w *w = &e->w;

    for (uint32_t i = 0; i < 9; i++) {
        if (i) wr_ch(w, (char)e->o->csv_delimiter);
        wr_str(w, k_meta[i]);
    }
    if (e->o->include_values) {
        for (uint32_t c = 0; c < e->d->n_columns; c++) {
            wr_ch(w, (char)e->o->csv_delimiter);
            /* A column name is file content, so it is escaped and quoted like any
             * other value rather than pasted in. */
            wr_ch(w, '"');
            wr_col_name(e, c, ESC_CSV);
            wr_ch(w, '"');
        }
    }
    wr_ch(w, '\n');
}

/* One CSV line. `side` selects which of the two rows the values come from, so a
 * modified row is two lines and the reader can see what it was and what it
 * became without the report having to invent an "old -> new" syntax that nothing
 * can parse back. */
static void emit_csv_line(emitter *e, const ibha_csvd_row *r, int source_side) {
    emit_w *w = &e->w;
    const ibha_csvd_table *t = source_side ? e->d->src : e->d->tgt;
    uint32_t row = source_side ? r->source_row : r->target_row;
    char sep = (char)e->o->csv_delimiter;

    wr_u64(w, (uint64_t)IBHA_CSVD_SCHEMA_VERSION);
    wr_ch(w, sep);
    csv_text(e, k_kind[r->kind & 3u]);
    wr_ch(w, sep);
    csv_text(e, source_side ? "source" : "target");
    wr_ch(w, sep);
    csv_text(e, r->moved ? "true" : "false");
    wr_ch(w, sep);
    wr_i32(w, r->move_distance);
    wr_ch(w, sep);
    if (r->source_row != IBHA_CSVD_NO_ROW) wr_u64(w, row_number(r->source_row));
    wr_ch(w, sep);
    if (r->target_row != IBHA_CSVD_NO_ROW) wr_u64(w, row_number(r->target_row));
    wr_ch(w, sep);
    wr_u64(w, r->n_changed_cells);
    wr_ch(w, sep);
    wr_u64(w, r->n_findings);

    if (e->o->include_values) {
        for (uint32_t c = 0; c < r->n_columns; c++) {
            wr_ch(w, sep);
            csv_field(e, t, row_field(t, row, c));
        }
    }
    wr_ch(w, '\n');
}

/* ------------------------------------------------------------------- html -- */

static void wr_cls(emitter *e, const char *suffix, int first) {
    if (!first) wr_ch(&e->w, ' ');
    wr_str(&e->w, e->prefix);
    wr_str(&e->w, suffix);
}

/* The five names a cell can carry, all compiled in. */
static const char *finding_class(uint8_t flags) {
    if (flags & IBHA_CSVD_CELL_REQUIRED_EMPTY) return "requiredEmpty";
    if (flags & IBHA_CSVD_CELL_TOO_LONG) return "tooLong";
    if (flags & IBHA_CSVD_CELL_NOT_NUMERIC) return "notNumeric";
    return "precision";
}

/*
 * One side of a changed cell, with the intra cell segments highlighted when the
 * caller asked for them. Segments come back as [op, start, len] over the logical
 * values, and EQUAL offsets index the source, so the target offset is tracked
 * alongside as the walk advances.
 */
static void html_cell_side(emitter *e, const ibha_csvd_row *r, uint32_t col, int source_side,
                           const ibha_csvd_segment *segs, int n) {
    const ibha_csvd_table *t = source_side ? e->d->src : e->d->tgt;
    uint32_t row = source_side ? r->source_row : r->target_row;
    uint32_t f = row_field(t, row, col);
    if (f == 0xFFFFFFFFu) return;

    if (n <= 0) {
        wr_value(&e->w, t, f, ESC_HTML, 0, 0, e->o->max_cell_bytes, NULL);
        return;
    }

    uint32_t soff = 0, toff = 0;
    for (int i = 0; i < n; i++) {
        uint32_t op = segs[i].op;
        uint32_t start = source_side ? soff : toff;
        if (op == IBHA_CSVD_SEG_EQUAL) {
            wr_value(&e->w, t, f, ESC_HTML, start, segs[i].len, 0, NULL);
            soff += segs[i].len;
            toff += segs[i].len;
        } else if (op == IBHA_CSVD_SEG_DELETE) {
            if (source_side) {
                wr_str(&e->w, "<del class=\"");
                wr_cls(e, "del", 1);
                wr_str(&e->w, "\">");
                wr_value(&e->w, t, f, ESC_HTML, soff, segs[i].len, 0, NULL);
                wr_str(&e->w, "</del>");
            }
            soff += segs[i].len;
        } else {
            if (!source_side) {
                wr_str(&e->w, "<ins class=\"");
                wr_cls(e, "ins", 1);
                wr_str(&e->w, "\">");
                wr_value(&e->w, t, f, ESC_HTML, toff, segs[i].len, 0, NULL);
                wr_str(&e->w, "</ins>");
            }
            toff += segs[i].len;
        }
    }
}

static void emit_html_row(emitter *e, const ibha_csvd_row *r) {
    emit_w *w = &e->w;

    wr_str(w, "<tr class=\"");
    wr_cls(e, "row", 1);
    wr_cls(e, k_kind[r->kind & 3u], 0);
    if (r->moved) wr_cls(e, "moved", 0);
    if (r->n_findings) wr_cls(e, "finding", 0);
    wr_str(w, "\"><td class=\"");
    wr_cls(e, "num", 1);
    wr_str(w, "\">");
    if (r->target_row != IBHA_CSVD_NO_ROW) {
        wr_u64(w, row_number(r->target_row));
    } else if (r->source_row != IBHA_CSVD_NO_ROW) {
        wr_u64(w, row_number(r->source_row));
    }
    wr_str(w, "</td>");

    if (!e->o->include_values) {
        wr_str(w, "</tr>\n");
        return;
    }

    for (uint32_t c = 0; c < r->n_columns; c++) {
        uint8_t fl = r->cell_flags[c];
        uint32_t fs = row_field(e->d->src, r->source_row, c);
        uint32_t ft = row_field(e->d->tgt, r->target_row, c);
        int changed = (fl & IBHA_CSVD_CELL_CHANGED) != 0;

        wr_str(w, "<td class=\"");
        wr_cls(e, "cell", 1);
        if (changed) wr_cls(e, "changed", 0);
        if (fl & IBHA_CSVD_CELL_SUPPRESSED) wr_cls(e, "suppressed", 0);
        if (fl & IBHA_CSVD_CELL_FINDING) wr_cls(e, "finding", 0);
        wr_ch(w, '"');
        if (fl & IBHA_CSVD_CELL_FINDING) {
            /* From a fixed compiled in set, never from the file. */
            wr_str(w, " data-finding=\"");
            wr_str(w, finding_class(fl));
            wr_ch(w, '"');
        }
        wr_ch(w, '>');

        if (changed && fs != 0xFFFFFFFFu && ft != 0xFFFFFFFFu) {
            ibha_csvd_segment segs[EMIT_MAX_SEGMENTS];
            int n = 0;
            if (e->o->cell_diff != IBHA_CSVD_CELLDIFF_NONE) {
                n = ibha_csvd_cell_segments(e->d, r, c,
                                            (ibha_csvd_cell_diff_mode)e->o->cell_diff,
                                            e->o->max_cell_bytes, segs, EMIT_MAX_SEGMENTS);
                if (n < 0 || (uint32_t)n > EMIT_MAX_SEGMENTS) n = 0;
            }
            wr_str(w, "<span class=\"");
            wr_cls(e, "old", 1);
            wr_str(w, "\">");
            html_cell_side(e, r, c, 1, segs, n);
            wr_str(w, "</span><span class=\"");
            wr_cls(e, "new", 1);
            wr_str(w, "\">");
            html_cell_side(e, r, c, 0, segs, n);
            wr_str(w, "</span>");
        } else if (ft != 0xFFFFFFFFu) {
            wr_value(w, e->d->tgt, ft, ESC_HTML, 0, 0, e->o->max_cell_bytes, NULL);
        } else if (fs != 0xFFFFFFFFu) {
            wr_value(w, e->d->src, fs, ESC_HTML, 0, 0, e->o->max_cell_bytes, NULL);
        }
        wr_str(w, "</td>");
    }
    wr_str(w, "</tr>\n");
}

static void emit_html_open(emitter *e) {
    emit_w *w = &e->w;
    wr_str(w, "<div class=\"");
    wr_cls(e, "report", 1);
    wr_str(w, "\" data-schema-version=\"");
    wr_u64(w, (uint64_t)IBHA_CSVD_SCHEMA_VERSION);
    wr_str(w, "\"><table class=\"");
    wr_cls(e, "table", 1);
    wr_str(w, "\">\n<thead><tr><th class=\"");
    wr_cls(e, "th", 1);
    wr_cls(e, "num", 0);
    wr_str(w, "\">row</th>");
    if (e->o->include_values) {
        for (uint32_t c = 0; c < e->d->n_columns; c++) {
            wr_str(w, "<th class=\"");
            wr_cls(e, "th", 1);
            wr_str(w, "\">");
            wr_col_name(e, c, ESC_HTML);
            wr_str(w, "</th>");
        }
    }
    wr_str(w, "</tr></thead>\n<tbody>\n");
}

static void emit_html_close(emitter *e) {
    wr_str(&e->w, "</tbody></table></div>\n");
}

/* ---------------------------------------------------------------- summary -- */

/*
 * Spec 13.8: where the uploaded file carries its own metadata rows and they
 * disagree with the source, the source wins and the disagreement is reported as a
 * finding rather than an error. This is where that finding surfaces.
 *
 * The comparison only makes sense when the two files have the same header
 * layout, which is exactly when the target's name row sits at the same index as
 * the source's. A names-only upload has no metadata to disagree with and produces
 * nothing.
 */
/* One column that is in the uploaded file and not the source, or the reverse.
 * The name comes out of the unprojected table, because a column that was added or
 * removed is by definition not in the compared set. */
static void summary_column_finding(emitter *e, const char *kind, const ibha_csvd_table *t,
                                   const ibha_csvd_schema *s, uint32_t col, int first) {
    emit_w *w = &e->w;
    if (!first) wr_ch(w, ',');
    wr_str(w, "{\"kind\":\"");
    wr_str(w, kind);
    wr_str(w, "\",\"column\":");
    wr_u64(w, col);
    wr_str(w, ",\"name\":\"");
    if (s->name_row != IBHA_CSVD_NO_ROW) {
        uint32_t f = ibha_csvd_row_field(t, s->name_row, col);
        if (f != 0xFFFFFFFFu) wr_value(w, t, f, ESC_JSON, 0, 0, 256, NULL);
    }
    wr_str(w, "\"}");
}

static void summary_schema_findings(emitter *e) {
    static const char *const k_row_name[3] = {"key", "required", "type"};
    emit_w *w = &e->w;
    const ibha_csvd_schema *ss = e->d->ss;
    const ibha_csvd_schema *ts = e->d->ts;
    uint32_t rows[3];
    uint32_t total = 0, listed = 0;
    int first = 1;

    wr_str(w, ",\"schemaFindings\":[");

    /*
     * Spec 6.6. These exist only when the caller allowed a column difference; the
     * default policy makes one an error and this loop never runs.
     */
    for (uint32_t i = 0; i < e->d->stats.columns_added; i++) {
        summary_column_finding(e, "columnAdded", e->d->tgt0, e->d->ts0, e->d->added_cols[i], first);
        first = 0;
        total++;
        listed++;
    }
    for (uint32_t i = 0; i < e->d->stats.columns_removed; i++) {
        summary_column_finding(e, "columnRemoved", e->d->src0, e->d->ss0, e->d->removed_cols[i],
                               first);
        first = 0;
        total++;
        listed++;
    }
    if (!e->d->projected && ts->name_row != IBHA_CSVD_NO_ROW && ts->name_row == ss->name_row &&
        ss->name_row > 0) {
        rows[0] = ss->key_row;
        rows[1] = ss->required_row;
        rows[2] = ss->type_row;

        for (uint32_t k = 0; k < 3; k++) {
            uint32_t row = rows[k];
            if (row == IBHA_CSVD_NO_ROW || row >= e->d->tgt->n_rows) continue;
            for (uint32_t c = 0; c < e->d->n_columns; c++) {
                uint32_t fs = ibha_csvd_row_field(e->d->src, row, c);
                uint32_t ft = ibha_csvd_row_field(e->d->tgt, row, c);
                if (fs == 0xFFFFFFFFu || ft == 0xFFFFFFFFu) continue;
                if (ibha_csvd_field_cmp(e->d->src, fs, e->d->tgt, ft) == 0) continue;

                total++;
                if (listed >= EMIT_MAX_SCHEMA_FINDINGS) continue;
                listed++;
                if (!first) wr_ch(w, ',');
                first = 0;
                wr_str(w, "{\"kind\":\"metadataDisagreement\",\"row\":\"");
                wr_str(w, k_row_name[k]);
                wr_str(w, "\",\"column\":");
                wr_u64(w, c);
                wr_str(w, ",\"name\":\"");
                wr_col_name(e, c, ESC_JSON);
                wr_str(w, "\",\"source\":\"");
                wr_value(w, e->d->src, fs, ESC_JSON, 0, 0, 256, NULL);
                wr_str(w, "\",\"target\":\"");
                wr_value(w, e->d->tgt, ft, ESC_JSON, 0, 0, 256, NULL);
                wr_str(w, "\"}");
            }
        }
    }
    wr_str(w, "],\"schemaFindingCount\":");
    wr_u64(w, total);
}

static void emit_summary(emitter *e) {
    emit_w *w = &e->w;
    const ibha_csvd_diff_stats *s = &e->d->stats;
    uint64_t findings = s->cells_required_empty + s->cells_too_long + s->cells_not_numeric +
                        s->cells_bad_precision;

    wr_str(w, "{\"schemaVersion\":");
    wr_u64(w, (uint64_t)IBHA_CSVD_SCHEMA_VERSION);
    wr_str(w, ",\"identical\":");
    wr_bool(w, !(s->rows_modified || s->rows_added || s->rows_deleted || s->rows_moved));

    wr_str(w, ",\"rows\":{\"unchanged\":");
    wr_u64(w, s->rows_unchanged);
    wr_str(w, ",\"modified\":");
    wr_u64(w, s->rows_modified);
    wr_str(w, ",\"added\":");
    wr_u64(w, s->rows_added);
    wr_str(w, ",\"deleted\":");
    wr_u64(w, s->rows_deleted);
    wr_str(w, ",\"moved\":");
    wr_u64(w, s->rows_moved);
    wr_str(w, ",\"report\":");
    wr_u64(w, s->report_rows);
    wr_ch(w, '}');

    wr_str(w, ",\"cells\":{\"changed\":");
    wr_u64(w, s->cells_changed);
    wr_str(w, ",\"suppressed\":");
    wr_u64(w, s->cells_suppressed);
    wr_ch(w, '}');

    wr_str(w, ",\"findings\":{\"total\":");
    wr_u64(w, findings);
    wr_str(w, ",\"rows\":");
    wr_u64(w, s->rows_with_findings);
    wr_str(w, ",\"requiredEmpty\":");
    wr_u64(w, s->cells_required_empty);
    wr_str(w, ",\"tooLong\":");
    wr_u64(w, s->cells_too_long);
    wr_str(w, ",\"notNumeric\":");
    wr_u64(w, s->cells_not_numeric);
    wr_str(w, ",\"precision\":");
    wr_u64(w, s->cells_bad_precision);
    wr_str(w, ",\"enabled\":");
    wr_bool(w, e->d->opts.validate);
    wr_ch(w, '}');

    wr_str(w, ",\"matching\":{\"allKeys\":");
    wr_bool(w, s->all_keys);
    wr_str(w, ",\"pairedBySimilarity\":");
    wr_u64(w, s->paired_by_similarity);
    wr_str(w, ",\"pairingTruncated\":");
    wr_bool(w, s->pairing_truncated);
    wr_str(w, ",\"movesForcedOff\":");
    wr_bool(w, s->moves_forced_off);
    wr_ch(w, '}');

    wr_str(w, ",\"columns\":{\"compared\":");
    wr_u64(w, s->n_columns_compared);
    wr_str(w, ",\"added\":");
    wr_u64(w, s->columns_added);
    wr_str(w, ",\"removed\":");
    wr_u64(w, s->columns_removed);
    wr_ch(w, '}');
    wr_str(w, ",\"targetHeader\":{\"rows\":");
    wr_u64(w, e->d->ts->first_data_row);
    wr_str(w, ",\"namesOnly\":");
    wr_bool(w, e->d->ts->names_only);
    wr_ch(w, '}');

    summary_schema_findings(e);
    wr_str(w, "}\n");
}

/* ------------------------------------------------------------ the driver -- */

void ibha_csvd_emit_opts_init(ibha_csvd_emit_opts *out, ibha_csvd_emit_format format) {
    if (!out) return;
    out->format = (uint8_t)format;
    out->changes_only = 0;
    out->include_values = 1;
    out->cell_diff = IBHA_CSVD_CELLDIFF_NONE;
    out->max_cell_bytes = 0;
    out->max_rows = 0;
    out->csv_formula_guard = 1;
    out->csv_delimiter = ',';
    out->class_prefix = NULL;
}

/*
 * The class prefix is the only caller supplied string that reaches the markup, so
 * it is checked rather than escaped: a prefix is an identifier, and a value that
 * is not one is a mistake worth failing on rather than mangling into something
 * that still renders.
 */
static int prefix_ok(const char *p) {
    if (!p || !*p) return 0;
    if (!((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) return 0;
    size_t n = IBHA_STRLEN(p);
    if (n > 32) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_';
        if (!ok) return 0;
    }
    return 1;
}

/* Rows that carry nothing to say, under changes_only. A finding on an otherwise
 * unchanged row is the point of the run, so it is never what this drops. */
static int row_is_quiet(const ibha_csvd_row *r) {
    return r->kind == IBHA_CSVD_ROW_UNCHANGED && !r->moved && r->n_suppressed_cells == 0 &&
           r->n_findings == 0;
}

ibha_csvd_status ibha_csvd_emit(ibha_csvd_diff *d, const ibha_csvd_emit_opts *opts,
                                const ibha_csvd_sink *sink, uint32_t *rows_written) {
    ibha_csvd_emit_opts local;
    emitter e;

    if (rows_written) *rows_written = 0;
    if (!d || !sink || !sink->write) return IBHA_CSVD_ERR_INVALID_ARG;
    if (d->ctx->status != IBHA_CSVD_OK) return d->ctx->status;

    if (opts) {
        local = *opts;
    } else {
        ibha_csvd_emit_opts_init(&local, IBHA_CSVD_EMIT_JSONL);
    }
    if (local.format > IBHA_CSVD_EMIT_SUMMARY) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_INVALID_ARG, "emit: unknown output format %u",
                        (unsigned)local.format);
    }
    if (local.cell_diff > IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_INVALID_ARG, "emit: unknown cell diff mode %u",
                        (unsigned)local.cell_diff);
    }
    if (local.csv_delimiter == 0) local.csv_delimiter = ',';
    if (local.class_prefix && !prefix_ok(local.class_prefix)) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_INVALID_ARG,
                        "emit: the HTML class prefix must match [A-Za-z][A-Za-z0-9_-]{0,31}");
    }

    e.d = d;
    e.o = &local;
    e.w.sink = sink;
    e.w.len = 0;
    e.w.err = 0;
    e.prefix = local.class_prefix ? local.class_prefix : "ibha-csvd-";
    e.name_row = d->ss->name_row;
    e.rows_written = 0;

    ibha_csvd_cursor *cur = ibha_csvd_cursor_open(d);
    if (!cur) return d->ctx->status;

    if (local.format == IBHA_CSVD_EMIT_SUMMARY) {
        /*
         * The cell level counters accumulate as a cursor advances, so a caller who
         * has already written JSONL would otherwise see the summary double count.
         * The summary is defined as the numbers of exactly one pass, so it starts
         * from zero and drains its own cursor.
         */
        d->stats.cells_changed = 0;
        d->stats.cells_suppressed = 0;
        d->stats.cells_required_empty = 0;
        d->stats.cells_too_long = 0;
        d->stats.cells_not_numeric = 0;
        d->stats.cells_bad_precision = 0;
        d->stats.rows_with_findings = 0;
        while (ibha_csvd_cursor_next(cur) == 1) e.rows_written++;
        emit_summary(&e);
        wr_flush(&e.w);
        if (rows_written) *rows_written = e.rows_written;
        return e.w.err ? ibha_err(d->ctx, IBHA_CSVD_ERR_IO, "emit: the sink refused a write")
                       : d->ctx->status;
    }

    if (local.format == IBHA_CSVD_EMIT_CSV) emit_csv_header(&e);
    if (local.format == IBHA_CSVD_EMIT_HTML) emit_html_open(&e);

    while (!e.w.err && ibha_csvd_cursor_next(cur) == 1) {
        const ibha_csvd_row *r = ibha_csvd_cursor_row(cur);
        if (local.changes_only && row_is_quiet(r)) continue;
        if (local.max_rows && e.rows_written >= local.max_rows) break;

        switch (local.format) {
            case IBHA_CSVD_EMIT_JSONL: emit_jsonl_row(&e, r); break;
            case IBHA_CSVD_EMIT_HTML: emit_html_row(&e, r); break;
            default:
                /* A modified row is two lines, what it was and what it became.
                 * A deleted row has only a source side and an added row only a
                 * target side, so each is one line. */
                if (r->kind == IBHA_CSVD_ROW_MODIFIED || r->kind == IBHA_CSVD_ROW_DELETED) {
                    emit_csv_line(&e, r, 1);
                }
                if (r->kind != IBHA_CSVD_ROW_DELETED) emit_csv_line(&e, r, 0);
                break;
        }
        e.rows_written++;
    }

    if (local.format == IBHA_CSVD_EMIT_HTML) emit_html_close(&e);
    wr_flush(&e.w);

    if (rows_written) *rows_written = e.rows_written;
    if (e.w.err) return ibha_err(d->ctx, IBHA_CSVD_ERR_IO, "emit: the sink refused a write");
    return d->ctx->status;
}
