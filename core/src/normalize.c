/*
 * normalize.c - the declared type comparators of spec 5.3.
 *
 * The business case this file exists for: a user downloads a CSV, opens it in
 * Excel, saves it and uploads it back. Excel has silently rewritten `00123` as
 * `123`, `1.50` as `1.5`, `TRUE` as `True` and a long number as `1.23457E+14`.
 * Nothing about the data changed. A byte comparison reports the whole file as
 * modified and the preview is useless.
 *
 * Header row 3, the declared types, is what stops that. It is not only
 * validation metadata: it selects a comparator.
 *
 * The design rule that matters more than any individual comparator:
 *
 *   **Every type reduces a cell to one normalized byte sequence, and exactly one
 *   pair of primitives consumes it.**
 *
 * ibha_norm_cmp and ibha_norm_hash are those primitives, and they are the Phase
 * 1 logical comparator and the Phase 1 logical hash unchanged. So the row
 * digests and the cell comparators cannot disagree: they are folded from the
 * same bytes. That is the whole reason the unchanged-row fast path of spec 6.1
 * step 3 is safe to take. Were the digest computed over raw bytes while the
 * comparator normalized, every row whose only change was 1.50 becoming 1.5 would
 * be reported as modified by the fast path and then found identical by the cell
 * walk, which is the worst kind of bug: self contradictory output.
 *
 * The normalized form is a *view* over the field's own bytes wherever it can be,
 * so the common case costs a pointer bump and nothing else. Only the numeric and
 * boolean canonical forms are built, and they are built into a fixed stack
 * buffer supplied by the caller. Nothing here allocates.
 */
#include "internal.h"

/* ------------------------------------------------------------- defaults -- */

static const char k_bool_true[] = "TRUE,T,YES,Y,1";
static const char k_bool_false[] = "FALSE,F,NO,N,0";

void ibha_csvd_compare_opts_init(ibha_csvd_compare_opts *out) {
    if (!out) return;
    out->trim_whitespace = 1;
    out->char_ignore_pad = 1;
    out->numeric = 1;
    out->booleans = 1;
    out->date_compare = IBHA_CSVD_DATE_EXACT;
    out->bool_true = NULL;
    out->bool_false = NULL;
    /* Spec 13.10: the uploaded file carries the same columns as the source, in
     * the same order. Both relaxations are opt in. */
    out->allow_added_columns = 0;
    out->allow_removed_columns = 0;
}

void ibha_compare_opts_resolve(ibha_csvd_compare_opts *o) {
    if (!o->bool_true) o->bool_true = k_bool_true;
    if (!o->bool_false) o->bool_false = k_bool_false;
    /* Normalized to 0 or 1 so that two callers who wrote 1 and 2 for "on" still
     * produce the same compare_id and can be diffed against each other. */
    o->trim_whitespace = o->trim_whitespace ? 1 : 0;
    o->char_ignore_pad = o->char_ignore_pad ? 1 : 0;
    o->numeric = o->numeric ? 1 : 0;
    o->booleans = o->booleans ? 1 : 0;
    o->allow_added_columns = o->allow_added_columns ? 1 : 0;
    o->allow_removed_columns = o->allow_removed_columns ? 1 : 0;
}

uint64_t ibha_compare_id(const ibha_csvd_compare_opts *o, const ibha_csvd_schema *s) {
    uint64_t h = 0;
    /* The column policy is in here because it changes which cells the digests are
     * folded from, so a pair parsed under different policies must not be
     * comparable. */
    uint8_t flags = (uint8_t)((o->trim_whitespace ? 1u : 0u) | (o->char_ignore_pad ? 2u : 0u) |
                              (o->numeric ? 4u : 0u) | (o->booleans ? 8u : 0u) |
                              (o->allow_added_columns ? 16u : 0u) |
                              (o->allow_removed_columns ? 32u : 0u));
    h = ibha_hash_mix(h, flags);
    h = ibha_hash_mix(h, (uint64_t)(uint32_t)o->date_compare);
    h = ibha_hash_mix(h, ibha_xxh3_64(o->bool_true, IBHA_STRLEN(o->bool_true)));
    h = ibha_hash_mix(h, ibha_xxh3_64(o->bool_false, IBHA_STRLEN(o->bool_false)));

    /* The schema facts a digest depends on. The key set decides what goes into
     * row_key_hash and the declared types decide which comparator each column
     * folds through, so a table digested under a different key set or a
     * different type row is not comparable with this one. */
    h = ibha_hash_mix(h, s->n_columns);
    h = ibha_hash_mix(h, s->n_key_columns);
    for (uint32_t c = 0; c < s->n_columns; c++) {
        h = ibha_hash_mix(h, s->col_flags ? s->col_flags[c] : 0u);
        h = ibha_hash_mix(h, s->col_type ? s->col_type[c] : 0u);
    }
    return ibha_hash_final(h, s->n_columns);
}

/* --------------------------------------------------------- declared types -- */

static int is_pad(uint8_t c) { return c == ' ' || c == '\t'; }

static uint8_t upper(uint8_t c) { return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c; }

static int str_eq_upper(const uint8_t *p, uint32_t len, const char *want) {
    for (uint32_t i = 0; i < len; i++) {
        if ((char)p[i] != want[i]) return 0;
    }
    return want[len] == '\0';
}

/*
 * Recognized type names. Matched on the identifier before any parenthesis, in
 * full rather than by prefix, because DATE is a prefix of DATETIME and getting
 * that wrong would silently pick the wrong comparator for a whole column.
 *
 * The float types map to DECIMAL deliberately. A column declared FLOAT still
 * holds decimal text in a CSV, and comparing it by canonical decimal value is
 * both exact and what the user means; parsing it to a binary double and
 * comparing that would reintroduce the representation error spec 5.3 is explicit
 * about avoiding.
 */
static uint8_t type_from_name(const uint8_t *p, uint32_t len) {
    if (len == 0) return IBHA_CSVD_TYPE_UNKNOWN;
    switch (p[0]) {
        case 'V':
            if (str_eq_upper(p, len, "VARCHAR")) return IBHA_CSVD_TYPE_VARCHAR;
            if (str_eq_upper(p, len, "VARCHAR2")) return IBHA_CSVD_TYPE_VARCHAR;
            break;
        case 'C':
            if (str_eq_upper(p, len, "CHAR")) return IBHA_CSVD_TYPE_CHAR;
            if (str_eq_upper(p, len, "CHARACTER")) return IBHA_CSVD_TYPE_CHAR;
            if (str_eq_upper(p, len, "CHARACTER VARYING")) return IBHA_CSVD_TYPE_VARCHAR;
            break;
        case 'T':
            if (str_eq_upper(p, len, "TEXT")) return IBHA_CSVD_TYPE_VARCHAR;
            if (str_eq_upper(p, len, "TINYINT")) return IBHA_CSVD_TYPE_INTEGER;
            if (str_eq_upper(p, len, "TIMESTAMP")) return IBHA_CSVD_TYPE_TIMESTAMP;
            break;
        case 'S':
            if (str_eq_upper(p, len, "STRING")) return IBHA_CSVD_TYPE_VARCHAR;
            if (str_eq_upper(p, len, "SMALLINT")) return IBHA_CSVD_TYPE_INTEGER;
            break;
        case 'D':
            if (str_eq_upper(p, len, "DECIMAL")) return IBHA_CSVD_TYPE_DECIMAL;
            if (str_eq_upper(p, len, "DOUBLE")) return IBHA_CSVD_TYPE_DECIMAL;
            if (str_eq_upper(p, len, "DOUBLE PRECISION")) return IBHA_CSVD_TYPE_DECIMAL;
            if (str_eq_upper(p, len, "DATE")) return IBHA_CSVD_TYPE_DATE;
            if (str_eq_upper(p, len, "DATETIME")) return IBHA_CSVD_TYPE_TIMESTAMP;
            break;
        case 'N':
            if (str_eq_upper(p, len, "NUMERIC")) return IBHA_CSVD_TYPE_DECIMAL;
            if (str_eq_upper(p, len, "NUMBER")) return IBHA_CSVD_TYPE_DECIMAL;
            break;
        case 'I':
            if (str_eq_upper(p, len, "INT")) return IBHA_CSVD_TYPE_INTEGER;
            if (str_eq_upper(p, len, "INTEGER")) return IBHA_CSVD_TYPE_INTEGER;
            break;
        case 'B':
            if (str_eq_upper(p, len, "BIGINT")) return IBHA_CSVD_TYPE_INTEGER;
            if (str_eq_upper(p, len, "BOOL")) return IBHA_CSVD_TYPE_BOOLEAN;
            if (str_eq_upper(p, len, "BOOLEAN")) return IBHA_CSVD_TYPE_BOOLEAN;
            break;
        case 'F':
            if (str_eq_upper(p, len, "FLOAT")) return IBHA_CSVD_TYPE_DECIMAL;
            break;
        case 'R':
            if (str_eq_upper(p, len, "REAL")) return IBHA_CSVD_TYPE_DECIMAL;
            break;
        default: break;
    }
    return IBHA_CSVD_TYPE_UNKNOWN;
}

/*
 * Reads one declared type cell. This is one of the very few places a value is
 * materialized, and it is a schema boundary rather than a per cell path: it runs
 * once per column, never per row.
 */
static void parse_type(const ibha_csvd_table *t, uint32_t f, uint8_t *out_type, int32_t *out_size,
                       int32_t *out_scale) {
    uint8_t buf[IBHA_CSVD_TYPE_TEXT_MAX + 1];
    *out_type = IBHA_CSVD_TYPE_UNKNOWN;
    *out_size = -1;
    *out_scale = -1;

    uint32_t need = ibha_csvd_field_logical_len(t, f);
    if (need == 0 || need > IBHA_CSVD_TYPE_TEXT_MAX) return;
    uint32_t n = ibha_csvd_field_copy(t, f, buf, sizeof(buf));

    /* Trim, upper case, and collapse runs of padding to a single space so that
     * "character  varying" still matches "CHARACTER VARYING". */
    uint32_t s = 0, e = n;
    while (s < e && is_pad(buf[s])) s++;
    while (e > s && is_pad(buf[e - 1])) e--;

    uint32_t w = 0;
    uint32_t name_len = 0;
    int in_pad = 0;
    uint32_t i = s;
    for (; i < e; i++) {
        if (buf[i] == '(') break;
        if (is_pad(buf[i])) {
            in_pad = 1;
            continue;
        }
        if (in_pad && w) buf[w++] = ' ';
        in_pad = 0;
        buf[w++] = upper(buf[i]);
    }
    name_len = w;
    *out_type = type_from_name(buf, name_len);

    /* The (p) or (p,s) suffix. A malformed suffix leaves both at -1 rather than
     * being an error: the declared size takes no part in comparison, so a type
     * row nobody can parse must not stop a diff. */
    if (i < e && buf[i] == '(') {
        int32_t nums[2] = {-1, -1};
        int slot = 0;
        int have = 0;
        for (i++; i < e && slot < 2; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                if (!have) {
                    nums[slot] = 0;
                    have = 1;
                }
                if (nums[slot] < 100000000) nums[slot] = nums[slot] * 10 + (buf[i] - '0');
            } else if (buf[i] == ',') {
                slot++;
                have = 0;
            } else if (buf[i] == ')') {
                break;
            }
        }
        *out_size = nums[0];
        *out_scale = nums[1];
    }
}

ibha_csvd_status ibha_types_resolve(ibha_csvd_ctx *ctx, const ibha_csvd_table *tbl,
                                    const ibha_csvd_schema *inherit, const uint32_t *map,
                                    ibha_csvd_schema *out) {
    out->col_type = NULL;
    out->col_size = NULL;
    out->col_scale = NULL;
    if (out->n_columns == 0) return IBHA_CSVD_OK;

    out->col_type = (uint8_t *)ibha_arena_calloc(&ctx->arena, out->n_columns);
    out->col_size = (int32_t *)ibha_arena_alloc(&ctx->arena, (size_t)out->n_columns * 4);
    out->col_scale = (int32_t *)ibha_arena_alloc(&ctx->arena, (size_t)out->n_columns * 4);
    if (!out->col_type || !out->col_size || !out->col_scale) {
        return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "schema: cannot allocate %u column types",
                        out->n_columns);
    }

    /*
     * Spec 13.8: the source file is authoritative for every piece of schema
     * metadata, and the target's own opinion is never acted on. So the target
     * copies the values rather than reading its own type row, and it copies them
     * rather than aliasing the source's arrays because the two sides may live in
     * different contexts.
     */
    /*
     * With a map, the target's columns inherit by name rather than by position,
     * which is the column policy of spec 6.6: a column the source does not
     * declare inherits nothing and stays UNKNOWN, so it compares as trimmed bytes
     * if anything ever looks at it.
     */
    if (inherit && inherit->col_type && (map || inherit->n_columns == out->n_columns)) {
        for (uint32_t c = 0; c < out->n_columns; c++) {
            uint32_t from = map ? map[c] : c;
            if (from >= inherit->n_columns) {
                out->col_type[c] = IBHA_CSVD_TYPE_UNKNOWN;
                out->col_size[c] = -1;
                out->col_scale[c] = -1;
                continue;
            }
            out->col_type[c] = inherit->col_type[from];
            out->col_size[c] = inherit->col_size ? inherit->col_size[from] : -1;
            out->col_scale[c] = inherit->col_scale ? inherit->col_scale[from] : -1;
        }
        return IBHA_CSVD_OK;
    }

    if (out->type_row == IBHA_CSVD_NO_ROW || out->type_row >= tbl->n_rows) {
        for (uint32_t c = 0; c < out->n_columns; c++) {
            out->col_type[c] = IBHA_CSVD_TYPE_UNKNOWN;
            out->col_size[c] = -1;
            out->col_scale[c] = -1;
        }
        return IBHA_CSVD_OK;
    }

    uint32_t base = tbl->row_first_field[out->type_row];
    for (uint32_t c = 0; c < out->n_columns; c++) {
        parse_type(tbl, base + c, &out->col_type[c], &out->col_size[c], &out->col_scale[c]);
    }
    return IBHA_CSVD_OK;
}

/* --------------------------------------------------------------- row fold -- */

/*
 * The three row digests, folded from the same normalized bytes the comparators
 * walk. This is the one coupling the whole engine rests on: the unchanged-row
 * fast path of spec 6.1 step 3 decides from the digest alone and never looks at a
 * cell, which is only sound because the digest means exactly what the comparator
 * means.
 *
 * It lives here rather than in the parser because it has two callers. The parser
 * folds each row as it completes it; the column projection of spec 6.6 folds them
 * again over the columns the two files have in common. A second implementation
 * for the second caller would be a second chance to disagree with ibha_norm_cmp.
 */
void ibha_hash_row(const ibha_csvd_table *t, const ibha_csvd_schema *s,
                   const ibha_csvd_compare_opts *o, uint32_t row) {
    uint32_t base = t->row_first_field[row];
    uint32_t n = s->n_columns;
    uint8_t quote = t->quote;
    uint64_t full = 0, key = 0, raw = 0;
    uint8_t scratch[IBHA_NORM_SCRATCH];

    for (uint32_t c = 0; c < n; c++) {
        uint32_t f = base + c;
        ibha_norm nv;
        int altered = ibha_normalize(t, f, s->col_type[c], o, scratch, &nv);
        uint64_t h = ibha_norm_hash(&nv, quote);

        full = ibha_hash_mix(full, h);
        if (s->col_flags[c] & IBHA_CSVD_COL_KEY) key = ibha_hash_mix(key, h);
        /* Cells normalization did not touch, which is nearly all of them, reuse
         * the hash already computed rather than being hashed a second time. */
        raw = ibha_hash_mix(
            raw, altered ? ibha_field_hash_raw(t->bytes + t->field_off[f], t->field_len[f],
                                               t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE,
                                               quote)
                         : h);
    }

    t->row_full_hash[row] = ibha_hash_final(full, n);
    t->row_raw_hash[row] = ibha_hash_final(raw, n);
    /* With no key columns declared, every column is the key. Spec 6.4 calls this
     * the all-keys case and it is the matcher's problem, not the parser's; here
     * it just means the two digests coincide. */
    t->row_key_hash[row] =
        s->n_key_columns ? ibha_hash_final(key, s->n_key_columns) : t->row_full_hash[row];
}

/* ---------------------------------------------------------- the primitives -- */

int ibha_norm_cmp(const ibha_norm *a, const ibha_norm *b, uint8_t quote) {
    return ibha_field_cmp_raw(a->p, a->len, a->esc, b->p, b->len, b->esc, quote);
}

uint64_t ibha_norm_hash(const ibha_norm *a, uint8_t quote) {
    return ibha_field_hash_raw(a->p, a->len, a->esc, quote);
}

/* --------------------------------------------------------- canonical forms -- */

/*
 * The canonical decimal form.
 *
 * Spec 5.3 asks for a fixed point comparison at the declared scale, with a
 * normalized string comparison as the fallback beyond int64 range. This does the
 * fallback's job for every value instead, which is strictly more correct and one
 * code path rather than two: the canonical form is the value written in plain
 * decimal with no redundant zeros and no sign on zero, so
 *
 *     1.5 == 1.50 == 1.500 == +1.5     007 == 7     -0.00 == 0
 *     1.23457E+14 == 123457000000000
 *
 * and 1.555 stays distinct from 1.554, which a truncation to the declared scale
 * would have merged. Money compares exactly because nothing ever becomes a
 * float, which is spec 5.3's actual requirement.
 *
 * The declared scale takes no part in this. It is what a later phase's precision
 * violation finding is measured against, not what decides equality.
 *
 * Returns 0 when the value is not a number, or when its canonical form does not
 * fit the scratch buffer, and the caller then compares trimmed bytes.
 */
int ibha_canonical_decimal(const uint8_t *p, uint32_t len, uint8_t *dst, uint32_t *out_len) {
    uint32_t i = 0;
    int neg = 0;

    if (i < len && (p[i] == '+' || p[i] == '-')) {
        neg = (p[i] == '-');
        i++;
    }

    /* Collect the significant digits and where the point sits relative to their
     * right hand end, so that the exponent is a single integer to carry. */
    uint8_t digits[IBHA_NORM_SCRATCH];
    uint32_t nd = 0;
    int32_t point = 0; /* value = digits * 10^point */
    int any = 0;
    int lead = 1; /* still skipping leading zeros */

    for (; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
        any = 1;
        if (lead && p[i] == '0') continue;
        lead = 0;
        if (nd >= sizeof(digits)) return 0; /* more digits than we will render */
        digits[nd++] = p[i];
    }
    if (i < len && p[i] == '.') {
        i++;
        for (; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
            any = 1;
            /* A fractional digit cannot be dropped as a leading zero once a
             * significant digit has been seen, and while none has it only shifts
             * the exponent. */
            if (lead && p[i] == '0') {
                point--;
                continue;
            }
            lead = 0;
            if (nd >= sizeof(digits)) return 0;
            digits[nd++] = p[i];
            point--;
        }
    }
    if (!any) return 0;

    if (i < len && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        int eneg = 0;
        if (i < len && (p[i] == '+' || p[i] == '-')) {
            eneg = (p[i] == '-');
            i++;
        }
        if (i >= len || p[i] < '0' || p[i] > '9') return 0;
        int32_t ev = 0;
        for (; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
            if (ev > 100000) return 0; /* far outside anything we would render */
            ev = ev * 10 + (p[i] - '0');
        }
        point += eneg ? -ev : ev;
    }
    if (i != len) return 0; /* trailing junk: not a number */

    /* Trailing zeros carry no value, so drop them and let the exponent absorb it.
     * This is what makes 1.50 and 1.5 land on the same bytes. */
    while (nd > 0 && digits[nd - 1] == '0') {
        nd--;
        point++;
    }
    if (nd == 0) { /* every digit was a zero, so the value is zero, sign and all */
        dst[0] = '0';
        *out_len = 1;
        return 1;
    }

    uint32_t w = 0;
    if (neg) dst[w++] = '-';

    if (point >= 0) {
        /* An integer: the digits followed by `point` zeros. */
        if ((uint64_t)w + nd + (uint64_t)point > IBHA_NORM_SCRATCH) return 0;
        for (uint32_t k = 0; k < nd; k++) dst[w++] = digits[k];
        for (int32_t k = 0; k < point; k++) dst[w++] = '0';
    } else {
        uint32_t frac = (uint32_t)(-point);
        if (frac < nd) {
            uint32_t ip = nd - frac;
            if ((uint64_t)w + nd + 1 > IBHA_NORM_SCRATCH) return 0;
            for (uint32_t k = 0; k < ip; k++) dst[w++] = digits[k];
            dst[w++] = '.';
            for (uint32_t k = ip; k < nd; k++) dst[w++] = digits[k];
        } else {
            /* Smaller than one: "0." then the gap zeros then the digits. */
            uint32_t gap = frac - nd;
            if ((uint64_t)w + 2 + gap + nd > IBHA_NORM_SCRATCH) return 0;
            dst[w++] = '0';
            dst[w++] = '.';
            for (uint32_t k = 0; k < gap; k++) dst[w++] = '0';
            for (uint32_t k = 0; k < nd; k++) dst[w++] = digits[k];
        }
    }
    *out_len = w;
    return 1;
}

/*
 * The canonical TIMESTAMP form: the value with insignificant trailing zeros
 * removed from its fractional seconds, so
 *
 *     14:22:05 == 14:22:05.000 == 14:22:05.0     14:22:05.100 == 14:22:05.1
 *
 * This is ibha_canonical_decimal's rule applied to the fraction, and it is here
 * for the same reason: a trailing zero carries no value, and an all zero
 * fraction is no fraction at all. A driver that renders seconds only and an
 * export configured for fixed millisecond precision describe the same instant,
 * and a diff that called them different would report every row of a table as
 * modified.
 *
 * Deliberately narrow. **It does not parse the date**, so 31/01/2026 and
 * 2026-01-31 remain different values; that is IBHA_CSVD_DATE_VALUE, which is not
 * implemented. All this does is shorten a terminal run of digits.
 *
 * It declines unless the value ends in '.' followed by one or more digits and
 * nothing else, so an offset suffix such as 14:22:05.000+05:30 is left exactly
 * as it arrived rather than half understood. Trimming a fraction out of the
 * middle of a shape this does not model is how a comparator silently starts
 * matching values that differ.
 *
 * Returns 0 when there is nothing to change, and the caller then compares the
 * trimmed bytes, which are already the canonical form.
 */
int ibha_canonical_timestamp(const uint8_t *p, uint32_t len, uint8_t *dst, uint32_t *out_len) {
    if (len == 0 || len > IBHA_NORM_SCRATCH) return 0;

    /* Walk back over a terminal digit run to the '.' that introduces it. Any
     * other byte on the way means this is not a shape with a bare fraction. */
    uint32_t dot = len;
    for (uint32_t i = len; i > 0; i--) {
        if (p[i - 1] == '.') {
            dot = i - 1;
            break;
        }
        if (p[i - 1] < '0' || p[i - 1] > '9') return 0;
    }

    if (dot == len) return 0;     /* no '.': nothing to canonicalize */
    if (dot + 1 == len) return 0; /* a trailing '.' with no digits: leave it be */
    if (dot == 0) return 0;       /* nothing before the '.': not a timestamp */

    uint32_t end = len;
    while (end > dot + 1 && p[end - 1] == '0') end--;
    if (end == dot + 1) end = dot; /* every fractional digit was a zero */

    if (end == len) return 0; /* already canonical, so do not copy */

    for (uint32_t i = 0; i < end; i++) dst[i] = p[i];
    *out_len = end;
    return 1;
}

/* One entry of a comma separated truth set, matched case insensitively. */
static int in_set(const char *set, const uint8_t *p, uint32_t len) {
    const char *s = set;
    while (*s) {
        const char *e = s;
        while (*e && *e != ',') e++;
        uint32_t n = (uint32_t)(e - s);
        if (n == len) {
            uint32_t i = 0;
            while (i < len && upper((uint8_t)s[i]) == upper(p[i])) i++;
            if (i == len) return 1;
        }
        s = *e ? e + 1 : e;
    }
    return 0;
}

/* ------------------------------------------------------------- normalize -- */

int ibha_normalize(const ibha_csvd_table *t, uint32_t f, uint8_t type,
                   const ibha_csvd_compare_opts *o, uint8_t *scratch, ibha_norm *out) {
    const uint8_t *p = t->bytes + t->field_off[f];
    uint32_t len = t->field_len[f];
    int esc = t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE;
    uint32_t raw_len = len;

    /*
     * Trimming never splits a "" pair, because neither of its bytes is padding,
     * so it stays a plain range narrowing even on the escaped path.
     *
     * CHAR(n) drops trailing pad even when trimming is off, per spec 5.3: a fixed
     * width column that arrived space padded to its declared width is the same
     * value, and that is a property of the type rather than of the option.
     */
    int trim_lead = o->trim_whitespace;
    int trim_trail = o->trim_whitespace || (type == IBHA_CSVD_TYPE_CHAR && o->char_ignore_pad);

    if (trim_lead) {
        while (len && is_pad(p[0])) {
            p++;
            len--;
        }
    }
    if (trim_trail) {
        while (len && is_pad(p[len - 1])) len--;
    }
    int changed = (len != raw_len);

    /* A value carrying a literal quote is not a number and not a boolean, so the
     * canonical forms only ever run on the unescaped fast path and never have to
     * collapse "" while parsing. */
    if (!esc) {
        if (o->numeric && (type == IBHA_CSVD_TYPE_DECIMAL || type == IBHA_CSVD_TYPE_INTEGER)) {
            uint32_t n = 0;
            if (ibha_canonical_decimal(p, len, scratch, &n)) {
                if (n != len || IBHA_MEMCMP(scratch, p, n) != 0) changed = 1;
                out->p = scratch;
                out->len = n;
                out->esc = 0;
                return changed;
            }
        } else if (type == IBHA_CSVD_TYPE_TIMESTAMP) {
            /* Not gated on an option: two timestamps that differ only in trailing
             * fractional zeros are the same instant, which is what the type
             * means, rather than a preference about how to compare it. */
            uint32_t n = 0;
            if (ibha_canonical_timestamp(p, len, scratch, &n)) {
                out->p = scratch;
                out->len = n;
                out->esc = 0;
                return 1; /* it only reports success when it changed the bytes */
            }
        } else if (o->booleans && type == IBHA_CSVD_TYPE_BOOLEAN) {
            int is_true = in_set(o->bool_true, p, len);
            int is_false = !is_true && in_set(o->bool_false, p, len);
            if (is_true || is_false) {
                scratch[0] = is_true ? '1' : '0';
                if (len != 1 || scratch[0] != p[0]) changed = 1;
                out->p = scratch;
                out->len = 1;
                out->esc = 0;
                return changed;
            }
        }
    }

    /* VARCHAR, CHAR, DATE and TIMESTAMP under 'exact', unknown and absent types,
     * and anything the canonical forms declined: trimmed byte equality, which is
     * spec 5.3's row for "unknown or absent" and its fallback everywhere else. */
    out->p = p;
    out->len = len;
    out->esc = esc;
    return changed;
}

int ibha_csvd_field_cmp_typed(const ibha_csvd_table *ta, uint32_t fa, const ibha_csvd_table *tb,
                              uint32_t fb, ibha_csvd_type type,
                              const ibha_csvd_compare_opts *opts) {
    if (!ta || !tb || fa >= ta->n_fields || fb >= tb->n_fields) return 0;

    ibha_csvd_compare_opts o;
    if (opts) {
        o = *opts;
    } else {
        ibha_csvd_compare_opts_init(&o);
    }
    ibha_compare_opts_resolve(&o);

    uint8_t sa[IBHA_NORM_SCRATCH], sb[IBHA_NORM_SCRATCH];
    ibha_norm na, nb;
    (void)ibha_normalize(ta, fa, (uint8_t)type, &o, sa, &na);
    (void)ibha_normalize(tb, fb, (uint8_t)type, &o, sb, &nb);
    return ibha_norm_cmp(&na, &nb, ta->quote);
}
