/*
 * schema.c - the four header row model and target header auto-detection.
 *
 * Spec 13.8, restated: the source file is authoritative for every piece of
 * schema metadata. Which columns are KEY, which are REQUIRED and what the
 * declared types are all come from the source, and the target's opinion about
 * any of it is never acted on. The target may legitimately have been reduced by
 * a spreadsheet to nothing but its column name row, so the number of header rows
 * it carries is detected rather than assumed.
 *
 * Detection is not a guess. We already know the column names from the source, so
 * we look for the row that *is* those names, within the first eight rows. That
 * gives a definite answer or a definite error, never a heuristic.
 *
 * Nothing here materializes a column name except to put one in an error message,
 * where a name the user can read is the entire point.
 */
#include "internal.h"

#define NAME_MSG_CAP 48

/* ASCII whitespace only. A UTF-8 non breaking space is data, not padding, and
 * treating it as padding would silently change a value. */
static int is_pad(uint8_t c) { return c == ' ' || c == '\t'; }

static void field_trimmed(const ibha_csvd_table *t, uint32_t f, const uint8_t **out_p,
                          uint32_t *out_len) {
    const uint8_t *p = t->bytes + t->field_off[f];
    uint32_t len = t->field_len[f];
    while (len && is_pad(p[0])) {
        p++;
        len--;
    }
    while (len && is_pad(p[len - 1])) len--;
    *out_p = p;
    *out_len = len;
}

/* Column names compare on their logical value with padding trimmed, and case
 * sensitively: a column that changed case is a schema change, and quietly
 * accepting it is the kind of leniency that surprises someone later. */
static int name_eq(const ibha_csvd_table *ta, uint32_t fa, const ibha_csvd_table *tb, uint32_t fb) {
    const uint8_t *pa, *pb;
    uint32_t la, lb;
    field_trimmed(ta, fa, &pa, &la);
    field_trimmed(tb, fb, &pb, &lb);
    return ibha_field_cmp_raw(pa, la, ta->field_flags[fa] & IBHA_CSVD_FIELD_HAS_ESCAPE, pb, lb,
                              tb->field_flags[fb] & IBHA_CSVD_FIELD_HAS_ESCAPE, ta->quote) == 0;
}

/* Marker cells are matched case insensitively, because KEY, Key and key all
 * plainly mean the same thing and a case mismatch there is not a schema change. */
static int marker_eq(const ibha_csvd_table *t, uint32_t f, const char *want) {
    const uint8_t *p;
    uint32_t len;
    field_trimmed(t, f, &p, &len);

    size_t n = IBHA_STRLEN(want);
    if ((size_t)len != n) return 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32);
        if (c != (uint8_t)want[i]) return 0;
    }
    return 1;
}

/* Copies a column name into a message buffer, truncating rather than
 * overflowing. Only ever called on an error path. */
static void name_str(const ibha_csvd_table *t, uint32_t f, char *dst, size_t cap) {
    const uint8_t *p;
    uint32_t len;
    field_trimmed(t, f, &p, &len);

    size_t n = len < cap - 4 ? (size_t)len : cap - 4;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    if ((size_t)len > n) {
        dst[n] = '.';
        dst[n + 1] = '.';
        dst[n + 2] = '.';
        n += 3;
    }
    dst[n] = '\0';
}

/*
 * Matches this row's column names against the source's, allowing whatever the
 * column policy of spec 6.6 allows, and fills map[c] with the source column each
 * of this row's columns corresponds to (or IBHA_CSVD_NO_COLUMN).
 *
 * The matching is a merge of two ordered lists rather than a search, and that is
 * the whole point: **the common columns must appear in the same relative order in
 * both files.** Spec 13.10 has no flag for reordering and this does not add one,
 * so a file whose columns are the same set in a different order fails here even
 * with both flags on, exactly as it does with them off.
 *
 * Returns the number of columns matched, or -1 when the row is not the header at
 * all. map may be NULL when only the count is wanted.
 */
static int32_t match_names(const ibha_csvd_table *tbl, uint32_t row, const ibha_csvd_table *et,
                           const ibha_csvd_schema *es, const ibha_csvd_compare_opts *cmp,
                           uint32_t *map) {
    uint32_t a = tbl->row_first_field[row];
    uint32_t b = et->row_first_field[es->name_row];
    uint32_t si = 0, ti = 0, matched = 0;
    int allow_added = cmp && cmp->allow_added_columns;
    int allow_removed = cmp && cmp->allow_removed_columns;

    if (map) {
        for (uint32_t c = 0; c < tbl->n_columns; c++) map[c] = IBHA_CSVD_NO_COLUMN;
    }

    while (si < es->n_columns && ti < tbl->n_columns) {
        if (name_eq(tbl, a + ti, et, b + si)) {
            if (map) map[ti] = si;
            matched++;
            si++;
            ti++;
            continue;
        }
        /*
         * The names differ here, so one of three things happened. Look ahead in
         * each list for the other's current name: finding the source's name later
         * in the target means the target inserted a column, and the reverse means
         * it dropped one. Finding neither means this column was renamed, which is
         * not an addition or a removal and is never allowed.
         */
        uint32_t look;
        for (look = ti + 1; look < tbl->n_columns; look++) {
            if (name_eq(tbl, a + look, et, b + si)) break;
        }
        if (look < tbl->n_columns) {
            if (!allow_added) return -1;
            ti++; /* tbl column ti is an addition */
            continue;
        }
        for (look = si + 1; look < es->n_columns; look++) {
            if (name_eq(tbl, a + ti, et, b + look)) break;
        }
        if (look < es->n_columns) {
            if (!allow_removed) return -1;
            si++; /* source column si is missing from tbl */
            continue;
        }
        /*
         * Neither name appears anywhere ahead in the other file, so this is one
         * column dropped and a different one put in its place. By names alone that
         * is indistinguishable from a rename, and guessing which it was would be
         * exactly the kind of leniency that quietly compares the wrong cells: it
         * needs both flags, and it is reported as one removal and one addition,
         * which is what the file actually shows.
         */
        if (!allow_added || !allow_removed) return -1;
        si++;
        ti++;
    }
    if (ti < tbl->n_columns && !allow_added) return -1;
    if (si < es->n_columns && !allow_removed) return -1;
    if (matched == 0) return -1;

    /*
     * The merge only ever matches columns in order, so a column that moved comes
     * out of it as a removal from one place and an addition in another. That is a
     * reordering, which spec 13.10 refuses with no flag, so it has to be told
     * apart from a genuine addition here rather than silently dropping the moved
     * column out of the comparison.
     *
     * The test is exact: an unmatched source column whose name exists anywhere in
     * the uploaded file did not disappear, it moved. It needs the map, so the
     * callers that decide whether to reject supply one; the caller that is only
     * locating the header row does not, and a reordered row is then rejected by
     * the resolve that follows it rather than by the search.
     */
    if (map) {
        for (uint32_t s = 0; s < es->n_columns; s++) {
            int is_matched = 0;
            for (uint32_t t = 0; t < tbl->n_columns && !is_matched; t++) {
                if (map[t] == s) is_matched = 1;
            }
            if (is_matched) continue;
            for (uint32_t t = 0; t < tbl->n_columns; t++) {
                if (name_eq(tbl, a + t, et, b + s)) return -1; /* it moved */
            }
        }
    }
    return (int32_t)matched;
}

int ibha_schema_row_matches_names(const ibha_csvd_table *tbl, uint32_t row,
                                  const ibha_csvd_table *expect_tbl,
                                  const ibha_csvd_schema *expect_schema,
                                  const ibha_csvd_compare_opts *cmp) {
    if (!tbl || !expect_tbl || !expect_schema) return 0;
    if (expect_schema->name_row == IBHA_CSVD_NO_ROW) return 0;
    if (row >= tbl->n_rows) return 0;
    if (expect_schema->n_columns == 0) return 0;

    int lenient = cmp && (cmp->allow_added_columns || cmp->allow_removed_columns);
    if (!lenient) {
        if (tbl->n_columns != expect_schema->n_columns) return 0;
        uint32_t a = tbl->row_first_field[row];
        uint32_t b = expect_tbl->row_first_field[expect_schema->name_row];
        for (uint32_t c = 0; c < expect_schema->n_columns; c++) {
            if (!name_eq(tbl, a + c, expect_tbl, b + c)) return 0;
        }
        return 1;
    }

    /*
     * Under a lenient policy a header row is one that carries every source column
     * it is going to carry, in order. Requiring at least one match keeps a data
     * row from being mistaken for the header when every name has been dropped.
     */
    int32_t n = match_names(tbl, row, expect_tbl, expect_schema, cmp, NULL);
    return n > 0;
}

uint32_t ibha_schema_column_map(const ibha_csvd_table *tbl, uint32_t row,
                                const ibha_csvd_table *expect_tbl,
                                const ibha_csvd_schema *expect_schema,
                                const ibha_csvd_compare_opts *cmp, uint32_t *map) {
    int32_t n = match_names(tbl, row, expect_tbl, expect_schema, cmp, map);
    return n < 0 ? 0u : (uint32_t)n;
}

/* True when the row holds the same set of names in a different order, which is
 * worth telling apart from "this is not the header row at all". */
static int row_is_permutation(const ibha_csvd_table *tbl, uint32_t row,
                              const ibha_csvd_table *et, const ibha_csvd_schema *es) {
    if (tbl->n_columns != es->n_columns || es->n_columns == 0) return 0;
    uint32_t a = tbl->row_first_field[row];
    uint32_t b = et->row_first_field[es->name_row];

    for (uint32_t c = 0; c < es->n_columns; c++) {
        int found = 0;
        for (uint32_t d = 0; d < es->n_columns && !found; d++) {
            if (name_eq(et, b + c, tbl, a + d)) found = 1;
        }
        if (!found) return 0;
    }
    return 1;
}

/*
 * Compares one row against the source's column names and reports the most
 * specific error it can. Column order is a hard error with no flag (spec 13.10),
 * and added or removed columns are errors by the same reasoning, so the useful
 * work here is telling the user which of those three things happened.
 */
static ibha_csvd_status column_mismatch(ibha_csvd_ctx *ctx, const ibha_csvd_table *tbl,
                                        uint32_t row, const ibha_csvd_table *et,
                                        const ibha_csvd_schema *es,
                                        const ibha_csvd_compare_opts *cmp) {
    /*
     * Under a lenient policy the additions and removals are not a mismatch at
     * all: they are findings, and the diff reports them. Anything match_names
     * still rejects, a reordering or a column swapped for a differently named one
     * without both flags, falls through to the messages below.
     *
     * The map is supplied rather than passed as NULL because the reorder check
     * needs it, and catching a reordered header here gives the user the specific
     * message below instead of a generic one from the diff.
     */
    if (cmp && (cmp->allow_added_columns || cmp->allow_removed_columns)) {
        uint32_t *map = (uint32_t *)ibha_arena_alloc(&ctx->arena, ((size_t)tbl->n_columns + 1) * 4);
        if (map && match_names(tbl, row, et, es, cmp, map) > 0) return IBHA_CSVD_OK;
    }

    if (tbl->n_columns != es->n_columns) {
        int lenient = cmp && (cmp->allow_added_columns || cmp->allow_removed_columns);
        return ibha_err(ctx, IBHA_CSVD_ERR_COLUMN_ORDER,
                        "the uploaded file has %u columns, the source has %u; %s",
                        tbl->n_columns, es->n_columns,
                        lenient ? "the columns they share are not in the same order"
                                : "adding or removing a column is not allowed");
    }

    uint32_t a = tbl->row_first_field[row];
    uint32_t b = et->row_first_field[es->name_row];
    for (uint32_t c = 0; c < es->n_columns; c++) {
        if (name_eq(tbl, a + c, et, b + c)) continue;

        char got[NAME_MSG_CAP], want[NAME_MSG_CAP];
        name_str(tbl, a + c, got, sizeof(got));
        name_str(et, b + c, want, sizeof(want));

        if (row_is_permutation(tbl, row, et, es)) {
            return ibha_err(ctx, IBHA_CSVD_ERR_COLUMN_ORDER,
                            "column %u of the uploaded file is \"%s\", expected \"%s\"; the columns "
                            "are the same but reordered, which is not allowed",
                            c + 1, got, want);
        }
        return ibha_err(ctx, IBHA_CSVD_ERR_COLUMN_ORDER,
                        "column %u of the uploaded file is \"%s\", expected \"%s\"", c + 1, got,
                        want);
    }
    return IBHA_CSVD_OK;
}

/* How many of a row's cells are the expected column name in the expected place. */
static uint32_t positional_matches(const ibha_csvd_table *tbl, uint32_t row,
                                   const ibha_csvd_table *et, const ibha_csvd_schema *es) {
    if (tbl->n_columns != es->n_columns) return 0;
    uint32_t a = tbl->row_first_field[row];
    uint32_t b = et->row_first_field[es->name_row];
    uint32_t n = 0;
    for (uint32_t c = 0; c < es->n_columns; c++) {
        if (name_eq(tbl, a + c, et, b + c)) n++;
    }
    return n;
}

ibha_csvd_status ibha_schema_no_header(ibha_csvd_ctx *ctx, const ibha_csvd_table *tbl,
                                       const ibha_csvd_table *et, const ibha_csvd_schema *es,
                                       const ibha_csvd_compare_opts *cmp) {
    if (!et || !es) {
        return ibha_err(ctx, IBHA_CSVD_ERR_NO_HEADER, "no source schema to locate a header against");
    }
    uint32_t scanned = tbl->n_rows < IBHA_CSVD_HEADER_SCAN_ROWS ? tbl->n_rows
                                                                : IBHA_CSVD_HEADER_SCAN_ROWS;

    /*
     * "Could not find the header" is the least useful thing we could say when
     * the header is plainly there and one column was renamed, reordered or
     * added. So pick the row that most looks like the header and report what is
     * actually wrong with it. Only a file with no resemblance to the expected
     * columns falls through to NO_HEADER.
     */
    if (tbl->n_columns != es->n_columns && tbl->n_rows > 0) {
        return column_mismatch(ctx, tbl, 0, et, es, cmp);
    }
    uint32_t best = 0, best_score = 0;
    for (uint32_t r = 0; r < scanned; r++) {
        uint32_t score = positional_matches(tbl, r, et, es);
        if (row_is_permutation(tbl, r, et, es)) score = es->n_columns;
        if (score > best_score) {
            best_score = score;
            best = r;
        }
    }
    if (best_score > 0) return column_mismatch(ctx, tbl, best, et, es, cmp);

    return ibha_err(ctx, IBHA_CSVD_ERR_NO_HEADER,
                    "could not locate the column header row in the uploaded file; expected a row "
                    "matching the source columns within the first %u rows",
                    IBHA_CSVD_HEADER_SCAN_ROWS);
}

ibha_csvd_status ibha_schema_resolve(ibha_csvd_ctx *ctx, const ibha_csvd_table *tbl,
                                     const ibha_csvd_header_opts *opt,
                                     const ibha_csvd_table *expect_tbl,
                                     const ibha_csvd_schema *expect_schema, uint32_t header_rows,
                                     const ibha_csvd_compare_opts *cmp, ibha_csvd_schema *out) {
    if (header_rows == IBHA_CSVD_HEADER_AUTO) {
        return ibha_schema_no_header(ctx, tbl, expect_tbl, expect_schema, cmp);
    }
    if (tbl->n_rows < header_rows) {
        return ibha_err(ctx, IBHA_CSVD_ERR_NO_HEADER,
                        "expected %u header rows, the file has %u rows", header_rows, tbl->n_rows);
    }

    out->n_columns = tbl->n_columns;
    out->n_key_columns = 0;
    out->first_data_row = header_rows;
    out->names_only = 0;
    out->key_row = IBHA_CSVD_NO_ROW;
    out->required_row = IBHA_CSVD_NO_ROW;
    out->type_row = IBHA_CSVD_NO_ROW;
    out->name_row = IBHA_CSVD_NO_ROW;

    if (opt->name_row >= 1 && opt->name_row <= header_rows) {
        out->name_row = opt->name_row - 1;
    } else if (expect_schema && header_rows >= 1) {
        /* Auto-detected: the row we matched is by construction the last header
         * row, whatever the source's own layout happens to be. */
        out->name_row = header_rows - 1;
    }

    out->col_flags = NULL;
    if (out->n_columns) {
        out->col_flags = (uint8_t *)ibha_arena_calloc(&ctx->arena, out->n_columns);
        if (!out->col_flags) {
            return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "schema: cannot allocate %u column flags",
                            out->n_columns);
        }
    }

    if (expect_schema) {
        /* The target inherits everything. Its own metadata rows, if it has any,
         * are skipped rather than read: spec 13.8 is explicit that the uploaded
         * file's opinion about which column is a key is not something to act on. */
        if (out->name_row == IBHA_CSVD_NO_ROW) {
            return ibha_err(ctx, IBHA_CSVD_ERR_NO_HEADER,
                            "the uploaded file has no column name row to check against the source");
        }
        ibha_csvd_status st =
            column_mismatch(ctx, tbl, out->name_row, expect_tbl, expect_schema, cmp);
        if (st != IBHA_CSVD_OK) return st;

        /*
         * Under the default policy the two files carry the same columns in the
         * same order, so the target inherits the source's metadata by position.
         * Under a lenient policy it inherits by *name*, and a column the source
         * does not declare inherits nothing: no flags, no declared type, which is
         * what makes it compare as trimmed bytes if anything ever looks at it.
         */
        uint32_t *map = NULL;
        if (cmp && (cmp->allow_added_columns || cmp->allow_removed_columns) &&
            out->n_columns != expect_schema->n_columns) {
            map = (uint32_t *)ibha_arena_alloc(&ctx->arena, (size_t)out->n_columns * 4);
            if (!map) {
                return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "schema: cannot allocate the column map");
            }
            (void)ibha_schema_column_map(tbl, out->name_row, expect_tbl, expect_schema, cmp, map);
        }

        for (uint32_t c = 0; c < out->n_columns; c++) {
            uint32_t src_col = map ? map[c] : c;
            uint8_t flags = src_col < expect_schema->n_columns ? expect_schema->col_flags[src_col]
                                                               : (uint8_t)0;
            out->col_flags[c] = flags;
            if (flags & IBHA_CSVD_COL_KEY) out->n_key_columns++;
        }
        out->names_only = (out->name_row == 0);
        return ibha_types_resolve(ctx, tbl, expect_schema, map, out);
    }

    if (opt->key_row >= 1 && opt->key_row <= header_rows) out->key_row = opt->key_row - 1;
    if (opt->required_row >= 1 && opt->required_row <= header_rows) {
        out->required_row = opt->required_row - 1;
    }
    if (opt->type_row >= 1 && opt->type_row <= header_rows) out->type_row = opt->type_row - 1;

    if (out->key_row != IBHA_CSVD_NO_ROW) {
        uint32_t base = tbl->row_first_field[out->key_row];
        for (uint32_t c = 0; c < out->n_columns; c++) {
            /* A cell that is neither empty nor the marker means "not a key"
             * rather than being an error: header rows carry comments and units
             * in real files, and rejecting those would be gratuitous. */
            if (marker_eq(tbl, base + c, "KEY")) {
                out->col_flags[c] |= IBHA_CSVD_COL_KEY;
                out->n_key_columns++;
            }
        }
    }
    if (out->required_row != IBHA_CSVD_NO_ROW) {
        uint32_t base = tbl->row_first_field[out->required_row];
        for (uint32_t c = 0; c < out->n_columns; c++) {
            if (marker_eq(tbl, base + c, "REQUIRED")) out->col_flags[c] |= IBHA_CSVD_COL_REQUIRED;
        }
    }
    /* Header row 3 is not only validation metadata: spec 5.3 has it select a
     * comparator, so it is parsed here, once per column, rather than per cell. */
    return ibha_types_resolve(ctx, tbl, NULL, NULL, out);
}
