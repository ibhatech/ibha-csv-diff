/*
 * validate.c - the validation findings of spec 13.5.
 *
 * The distinction this file exists to hold up: **errors abort the diff, findings
 * are output.** A malformed quote or a duplicate key means the file cannot be
 * compared, so the context records it and everything stops. A REQUIRED column
 * with an empty cell, a value longer than its VARCHAR(n), a value that does not
 * parse as its DECIMAL(p,s): those are *the point of running the comparison*.
 * Aborting on the first one would hide the other four hundred and make the
 * feature useless, so they flow through the cursor as flags on the affected cell
 * and are counted in the summary.
 *
 * Two decisions worth knowing before changing anything here.
 *
 * **The source file's schema is what a cell is measured against**, per spec 13.8.
 * The uploaded file's own opinion about which column is REQUIRED is never acted
 * on, so a salesman who deleted the metadata rows cannot turn a finding off.
 *
 * **"Does not parse as a number" means exactly what the comparator means by a
 * number.** The scanner here is ibha_canonical_decimal, which is the one the
 * comparators of spec 5.3 use. Writing a second scanner would let a cell be
 * reported as unparseable in the findings and compared as a number in the same
 * report, which is the self contradictory output the whole engine is arranged to
 * avoid.
 *
 * The cost model: a per column plan is built once, and a schema that declares no
 * REQUIRED column, no length and no numeric type sets checks_any to 0, after
 * which the cursor never reads a cell it would not otherwise have read.
 */
#include "internal.h"

ibha_csvd_status ibha_validate_plan(ibha_csvd_diff *d) {
    d->col_check = NULL;
    d->checks_any = 0;
    if (!d->opts.validate || d->n_columns == 0) return IBHA_CSVD_OK;

    const ibha_csvd_schema *s = d->ss;
    d->col_check = (uint8_t *)ibha_arena_calloc(&d->ctx->arena, d->n_columns);
    if (!d->col_check) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the validation plan");
    }

    for (uint32_t c = 0; c < d->n_columns; c++) {
        uint8_t m = 0;
        if (s->col_flags && (s->col_flags[c] & IBHA_CSVD_COL_REQUIRED)) m |= IBHA_CHK_REQUIRED;
        if (s->col_type) {
            uint8_t t = s->col_type[c];
            int32_t size = s->col_size ? s->col_size[c] : -1;
            if ((t == IBHA_CSVD_TYPE_VARCHAR || t == IBHA_CSVD_TYPE_CHAR) && size > 0) {
                m |= IBHA_CHK_LENGTH;
            }
            if (t == IBHA_CSVD_TYPE_DECIMAL || t == IBHA_CSVD_TYPE_INTEGER) m |= IBHA_CHK_NUMERIC;
        }
        d->col_check[c] = m;
        if (m) d->checks_any = 1;
    }
    return IBHA_CSVD_OK;
}

/* ------------------------------------------------------------ primitives -- */

static int is_pad(uint8_t c) { return c == ' ' || c == '\t'; }

/*
 * Characters, not bytes, because VARCHAR(10) is ten characters in every database
 * that would be receiving this file, and a name with an accent in it is not
 * suddenly too long. Counting UTF-8 lead bytes is exact for well formed UTF-8 and
 * degrades to something sensible for a file that is not: a Latin-1 byte counts as
 * one character, which is what it is.
 */
static uint32_t char_count(const uint8_t *p, uint32_t len, int esc, uint8_t quote) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (esc && p[i] == quote && i + 1 < len && p[i + 1] == quote) i++;
        if ((p[i] & 0xC0u) != 0x80u) n++;
    }
    return n;
}

/*
 * Digit counts of the canonical form, which is what the declared precision and
 * scale are measured against. The canonical form has already dropped redundant
 * zeros, so DECIMAL(4,2) accepts 1.50 and 1.5 alike, and rejects 1.555 on scale
 * and 100.5 on precision.
 */
static void digit_counts(const uint8_t *p, uint32_t len, uint32_t *ints, uint32_t *fracs) {
    uint32_t i = 0, ip = 0, fp = 0;
    int after_point = 0;
    if (i < len && p[i] == '-') i++;
    for (; i < len; i++) {
        if (p[i] == '.') {
            after_point = 1;
            continue;
        }
        if (after_point) {
            fp++;
        } else {
            ip++;
        }
    }
    *ints = ip;
    *fracs = fp;
}

/* ------------------------------------------------------------- one cell -- */

static uint8_t check_cell(const ibha_csvd_diff *d, const ibha_csvd_table *t, uint32_t f,
                          uint8_t checks, uint32_t col) {
    const uint8_t *p = t->bytes + t->field_off[f];
    uint32_t len = t->field_len[f];
    int esc = t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE;
    uint8_t found = 0;

    /* Trimmed exactly as the comparators trim, so "the value" means one thing
     * across the engine. A REQUIRED cell holding three spaces is empty under the
     * default settings and is a finding, and is not one when trimming is off. */
    if (d->opts.compare.trim_whitespace) {
        while (len && is_pad(p[0])) {
            p++;
            len--;
        }
        while (len && is_pad(p[len - 1])) len--;
    }

    if (len == 0) {
        /* An empty cell is absent, not malformed. Only a REQUIRED column has
         * anything to say about it: reporting an empty optional cell as an
         * unparseable number would bury the findings that matter in noise. */
        return (checks & IBHA_CHK_REQUIRED) ? (uint8_t)IBHA_CSVD_CELL_REQUIRED_EMPTY : 0u;
    }

    if (checks & IBHA_CHK_LENGTH) {
        uint32_t n = char_count(p, len, esc, t->quote);
        if (n > (uint32_t)d->ss->col_size[col]) found |= IBHA_CSVD_CELL_TOO_LONG;
    }

    if (checks & IBHA_CHK_NUMERIC) {
        uint8_t canon[IBHA_NORM_SCRATCH];
        uint32_t clen = 0;
        /* A value carrying a literal quote is not a number, and the canonical
         * form never has to collapse "" while parsing. */
        if (esc || !ibha_canonical_decimal(p, len, canon, &clen)) {
            found |= IBHA_CSVD_CELL_NOT_NUMERIC;
        } else {
            uint32_t ints = 0, fracs = 0;
            digit_counts(canon, clen, &ints, &fracs);

            int32_t size = d->ss->col_size[col];
            int32_t scale = d->ss->col_scale[col];
            if (d->ss->col_type[col] == IBHA_CSVD_TYPE_INTEGER) {
                /* An INTEGER column rarely declares a precision, and 1.5 in one is
                 * worth saying whether it does or not. */
                if (fracs > 0) found |= IBHA_CSVD_CELL_PRECISION;
                if (size > 0 && ints > (uint32_t)size) found |= IBHA_CSVD_CELL_PRECISION;
            } else if (size > 0) {
                /* SQL's rule: p total digits of which s are fractional, so the
                 * integer part has p - s to work with. An undeclared scale is
                 * zero, as it is in every DECIMAL(p) declaration. */
                uint32_t s = scale > 0 ? (uint32_t)scale : 0u;
                if (fracs > s) found |= IBHA_CSVD_CELL_PRECISION;
                if (s <= (uint32_t)size && ints > (uint32_t)size - s) {
                    found |= IBHA_CSVD_CELL_PRECISION;
                }
            }
        }
    }
    return found;
}

uint32_t ibha_validate_row(ibha_csvd_diff *d, const ibha_csvd_table *tbl, uint32_t row,
                           uint8_t *flags) {
    if (!d->checks_any || row >= tbl->n_rows) return 0;

    uint32_t base = tbl->row_first_field[row];
    uint32_t have = tbl->row_first_field[row + 1] - base;
    uint32_t n = d->n_columns < have ? d->n_columns : have;
    uint32_t rows_found = 0;

    for (uint32_t c = 0; c < n; c++) {
        uint8_t checks = d->col_check[c];
        if (!checks) continue;
        uint8_t found = check_cell(d, tbl, base + c, checks, c);
        if (!found) continue;

        flags[c] |= found;
        rows_found++;
        if (found & IBHA_CSVD_CELL_REQUIRED_EMPTY) d->stats.cells_required_empty++;
        if (found & IBHA_CSVD_CELL_TOO_LONG) d->stats.cells_too_long++;
        if (found & IBHA_CSVD_CELL_NOT_NUMERIC) d->stats.cells_not_numeric++;
        if (found & IBHA_CSVD_CELL_PRECISION) d->stats.cells_bad_precision++;
    }
    if (rows_found) d->stats.rows_with_findings++;
    return rows_found;
}
