/*
 * diff.c - the diff driver, the report order of spec 6.5 and the pull cursor of
 * spec 13.3.
 *
 * The output primitive is a cursor and everything else is an emitter built on
 * it. Two consequences shape this file.
 *
 * **Nothing is accumulated.** Peak memory is the two indexes plus one row.
 * Spec 6.5's flat report index array is not built at all: the deleted rows are
 * threaded onto per position buckets, which is O(rows) of `uint32_t` rather than
 * a materialized report, and the cursor walks the target rows in place. A
 * consumer that wants random access builds the index itself, which is what makes
 * it a cursor consumer rather than the primary interface.
 *
 * **Nothing is compared until it is pulled.** The matcher decides added,
 * deleted, modified, unchanged and moved without looking at a single cell, using
 * only the row digests. Finding out *which* cells changed is the expensive part
 * and it happens one row at a time, as the consumer asks. A caller that only
 * wants counts never pays for it.
 *
 * Report order, restated from spec 6.5: the report follows the modified file's
 * row order, which fully determines where unchanged, modified, added and moved
 * rows go. It says nothing about deleted rows, which have no position in the
 * target. `anchored` puts each one immediately after the target position of the
 * most recently matched source row, so a row deleted from the middle of the file
 * appears in the middle of the report next to its former neighbours.
 */
#include "internal.h"

#define NO_ROW IBHA_CSVD_NO_ROW

void ibha_csvd_diff_opts_init(ibha_csvd_diff_opts *out) {
    if (!out) return;
    ibha_csvd_compare_opts_init(&out->compare);
    out->detect_moves = 1;
    out->source_ordered = 1; /* spec 13.2: a CSV file always has an order */
    out->count_suppressed = 1;
    out->validate = 1;
    out->require_key = 0;
    out->deleted_placement = IBHA_CSVD_DELETED_ANCHORED;
    out->similarity_k = 16;
    out->similarity_percent = 50;
    out->max_pair_work = 4000000;
}

/* ------------------------------------------------------------ validation -- */

static ibha_csvd_status check_sides(ibha_csvd_ctx *ctx, const ibha_csvd_table *src,
                                    const ibha_csvd_schema *ss, const ibha_csvd_table *tgt,
                                    const ibha_csvd_schema *ts,
                                    const ibha_csvd_diff_opts *o) {
    if (!src->has_digests || !tgt->has_digests) {
        return ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG,
                        "diff: both sides must be parsed with row digests enabled");
    }
    if (ss->n_columns != ts->n_columns) {
        return ibha_err(ctx, IBHA_CSVD_ERR_COLUMN_ORDER,
                        "the uploaded file has %u columns, the source has %u", ts->n_columns,
                        ss->n_columns);
    }
    if (src->quote != tgt->quote) {
        return ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG,
                        "diff: the two sides were parsed with different quote characters");
    }

    /*
     * The digests on the two sides only mean the same thing if they were folded
     * through the same comparators. Comparing them across a settings mismatch
     * would not fail, it would produce a confidently wrong report, so the stamp
     * is checked rather than trusted.
     */
    uint64_t want = ibha_compare_id(&o->compare, ss);
    if (src->compare_id != want || tgt->compare_id != want) {
        return ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG,
                        "diff: the two sides were parsed under different comparison settings or "
                        "different schemas; digests from one are not comparable with the other");
    }
    if (o->require_key && ss->n_key_columns == 0) {
        return ibha_err(ctx, IBHA_CSVD_ERR_MISSING_KEY_COLUMN,
                        "diff: no column is marked KEY in the source file, and keyed matching was "
                        "required");
    }
    return IBHA_CSVD_OK;
}

/* --------------------------------------------- the column plan, spec 6.6 -- */

/*
 * When the uploaded file does not carry the same columns as the source and the
 * policy allows that, the diff compares the columns they have in common, in the
 * source's order, and reports the rest as findings.
 *
 * The implementation is deliberately a *projection* rather than a map threaded
 * through the engine. Column c means the same thing on both sides in a dozen
 * places, several of which compare two rows of the *same* side, and a per side
 * column map would have to reach all of them correctly. Instead each side gets a
 * table holding exactly the compared columns in the source's order, and every
 * other line in the engine keeps working unchanged because it is still true that
 * column c means the same thing on both sides.
 *
 * The cost is one rebuilt index per side, about 12 bytes per compared cell, and
 * it is paid only when the column sets actually differ. The default policy never
 * reaches this code.
 */

/* Rebuilds one side over `cols`, sharing the bytes, and refolds its digests
 * against the compared schema. Sharing the index arrays when a side contributes
 * all of its columns in order keeps the common "a column was added to the target"
 * case from copying the source's index for no reason. */
static int project_side(ibha_csvd_ctx *ctx, const ibha_csvd_table *in, const uint32_t *cols,
                        uint32_t n_cols, const ibha_csvd_schema *cs,
                        const ibha_csvd_compare_opts *o, uint32_t first_data_row,
                        ibha_csvd_table *out) {
    *out = *in;
    out->n_columns = n_cols;

    int identity = (n_cols == in->n_columns);
    for (uint32_t c = 0; identity && c < n_cols; c++) {
        if (cols[c] != c) identity = 0;
    }

    if (!identity) {
        size_t n_fields = (size_t)in->n_rows * n_cols;
        uint32_t *off = (uint32_t *)ibha_arena_alloc_large(&ctx->arena, (n_fields + 1) * 4);
        uint32_t *len = (uint32_t *)ibha_arena_alloc_large(&ctx->arena, (n_fields + 1) * 4);
        uint8_t *flags = (uint8_t *)ibha_arena_alloc_large(&ctx->arena, n_fields + 1);
        uint32_t *first = (uint32_t *)ibha_arena_alloc_large(&ctx->arena,
                                                             ((size_t)in->n_rows + 1) * 4);
        if (!off || !len || !flags || !first) return 0;

        uint32_t w = 0;
        for (uint32_t r = 0; r < in->n_rows; r++) {
            uint32_t base = in->row_first_field[r];
            first[r] = w;
            for (uint32_t c = 0; c < n_cols; c++) {
                uint32_t f = base + cols[c];
                off[w] = in->field_off[f];
                len[w] = in->field_len[f];
                flags[w] = in->field_flags[f];
                w++;
            }
        }
        first[in->n_rows] = w;
        out->field_off = off;
        out->field_len = len;
        out->field_flags = flags;
        out->row_first_field = first;
        out->n_fields = w;
    }

    /* Fresh digest arrays either way: the old ones were folded over a different
     * set of columns and mean nothing here. */
    uint64_t *kh = (uint64_t *)ibha_arena_alloc_large(&ctx->arena, ((size_t)in->n_rows + 1) * 8);
    uint64_t *fh = (uint64_t *)ibha_arena_alloc_large(&ctx->arena, ((size_t)in->n_rows + 1) * 8);
    uint64_t *rh = (uint64_t *)ibha_arena_alloc_large(&ctx->arena, ((size_t)in->n_rows + 1) * 8);
    if (!kh || !fh || !rh) return 0;
    out->row_key_hash = kh;
    out->row_full_hash = fh;
    out->row_raw_hash = rh;

    for (uint32_t r = 0; r < in->n_rows; r++) {
        if (r < first_data_row) {
            kh[r] = 0;
            fh[r] = 0;
            rh[r] = 0;
            continue;
        }
        ibha_hash_row(out, cs, o, r);
    }
    out->compare_id = ibha_compare_id(o, cs);
    return 1;
}

/* Copies the source's metadata for the compared columns, because spec 13.8 makes
 * the source authoritative for every piece of it. */
static int build_compare_schema(ibha_csvd_ctx *ctx, const ibha_csvd_schema *ss,
                                const ibha_csvd_schema *base, const uint32_t *src_cols,
                                uint32_t n_cols, ibha_csvd_schema *out) {
    *out = *base;
    out->n_columns = n_cols;
    out->n_key_columns = 0;
    out->col_flags = (uint8_t *)ibha_arena_calloc(&ctx->arena, n_cols + 1);
    out->col_type = (uint8_t *)ibha_arena_calloc(&ctx->arena, n_cols + 1);
    out->col_size = (int32_t *)ibha_arena_alloc(&ctx->arena, ((size_t)n_cols + 1) * 4);
    out->col_scale = (int32_t *)ibha_arena_alloc(&ctx->arena, ((size_t)n_cols + 1) * 4);
    if (!out->col_flags || !out->col_type || !out->col_size || !out->col_scale) return 0;

    for (uint32_t c = 0; c < n_cols; c++) {
        uint32_t s = src_cols[c];
        out->col_flags[c] = ss->col_flags ? ss->col_flags[s] : 0u;
        out->col_type[c] = ss->col_type ? ss->col_type[s] : (uint8_t)IBHA_CSVD_TYPE_UNKNOWN;
        out->col_size[c] = ss->col_size ? ss->col_size[s] : -1;
        out->col_scale[c] = ss->col_scale ? ss->col_scale[s] : -1;
        if (out->col_flags[c] & IBHA_CSVD_COL_KEY) out->n_key_columns++;
    }
    return 1;
}

static ibha_csvd_status plan_columns(ibha_csvd_diff *d) {
    const ibha_csvd_schema *ss = d->ss;
    const ibha_csvd_schema *ts = d->ts;
    ibha_csvd_ctx *ctx = d->ctx;

    d->stats.n_columns_compared = ss->n_columns;
    if (!d->opts.compare.allow_added_columns && !d->opts.compare.allow_removed_columns) {
        return IBHA_CSVD_OK; /* the default: check_sides insists the counts agree */
    }
    if (ss->n_columns == ts->n_columns && ss->name_row == IBHA_CSVD_NO_ROW) {
        return IBHA_CSVD_OK; /* nothing to reconcile and no names to do it with */
    }

    /*
     * Without a column name row there is no way to tell an added column from a
     * shifted one, and guessing would silently compare the wrong pairs of cells.
     * A count mismatch stays an error there, whatever the flags say.
     */
    if (ss->name_row == IBHA_CSVD_NO_ROW || ts->name_row == IBHA_CSVD_NO_ROW) {
        if (ss->n_columns == ts->n_columns) return IBHA_CSVD_OK;
        return ibha_err(ctx, IBHA_CSVD_ERR_COLUMN_ORDER,
                        "the uploaded file has %u columns, the source has %u, and neither carries a "
                        "column name row to reconcile them by",
                        ts->n_columns, ss->n_columns);
    }

    uint32_t *map = (uint32_t *)ibha_arena_alloc(&ctx->arena, ((size_t)ts->n_columns + 1) * 4);
    uint32_t *src_cols = (uint32_t *)ibha_arena_alloc(&ctx->arena, ((size_t)ss->n_columns + 1) * 4);
    uint32_t *tgt_cols = (uint32_t *)ibha_arena_alloc(&ctx->arena, ((size_t)ts->n_columns + 1) * 4);
    if (!map || !src_cols || !tgt_cols) {
        return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the column plan");
    }

    uint32_t matched = ibha_schema_column_map(d->tgt, ts->name_row, d->src, ss, &d->opts.compare,
                                              map);
    if (matched == 0) {
        return ibha_err(ctx, IBHA_CSVD_ERR_COLUMN_ORDER,
                        "the uploaded file's columns cannot be reconciled with the source's; they "
                        "share no column in the same order");
    }

    /* The map is a merge of two ordered lists, so walking the target's columns in
     * order visits the matched source columns in order too. */
    uint32_t n = 0;
    for (uint32_t t = 0; t < ts->n_columns; t++) {
        if (map[t] == IBHA_CSVD_NO_COLUMN) continue;
        src_cols[n] = map[t];
        tgt_cols[n] = t;
        n++;
    }

    d->stats.columns_added = ts->n_columns - n;
    d->stats.columns_removed = ss->n_columns - n;
    d->stats.n_columns_compared = n;
    if (d->stats.columns_added == 0 && d->stats.columns_removed == 0) return IBHA_CSVD_OK;

    d->added_cols = (uint32_t *)ibha_arena_alloc(&ctx->arena,
                                                 ((size_t)d->stats.columns_added + 1) * 4);
    d->removed_cols = (uint32_t *)ibha_arena_alloc(&ctx->arena,
                                                   ((size_t)d->stats.columns_removed + 1) * 4);
    if (!d->added_cols || !d->removed_cols) {
        return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the column findings");
    }
    uint32_t na = 0;
    for (uint32_t t = 0; t < ts->n_columns; t++) {
        if (map[t] == IBHA_CSVD_NO_COLUMN) d->added_cols[na++] = t;
    }
    uint32_t nr = 0, at = 0;
    for (uint32_t s = 0; s < ss->n_columns; s++) {
        if (at < n && src_cols[at] == s) {
            at++;
            continue;
        }
        d->removed_cols[nr++] = s;
    }

    /*
     * A missing KEY column is an error whatever the policy says. The key is what
     * row matching is built on, so tolerating its absence would not produce a
     * lenient diff, it would produce a meaningless one.
     */
    for (uint32_t i = 0; i < nr; i++) {
        if (ss->col_flags && (ss->col_flags[d->removed_cols[i]] & IBHA_CSVD_COL_KEY)) {
            return ibha_err(ctx, IBHA_CSVD_ERR_MISSING_KEY_COLUMN,
                            "column %u of the source is a KEY column and the uploaded file does not "
                            "carry it; rows cannot be matched without it",
                            d->removed_cols[i] + 1);
        }
    }

    ibha_csvd_schema *cs = (ibha_csvd_schema *)ibha_arena_calloc(&ctx->arena, sizeof(*cs));
    ibha_csvd_schema *cts = (ibha_csvd_schema *)ibha_arena_calloc(&ctx->arena, sizeof(*cts));
    ibha_csvd_table *psrc = (ibha_csvd_table *)ibha_arena_calloc(&ctx->arena, sizeof(*psrc));
    ibha_csvd_table *ptgt = (ibha_csvd_table *)ibha_arena_calloc(&ctx->arena, sizeof(*ptgt));
    if (!cs || !cts || !psrc || !ptgt ||
        !build_compare_schema(ctx, ss, ss, src_cols, n, cs) ||
        !build_compare_schema(ctx, ss, ts, src_cols, n, cts)) {
        return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the compared schema");
    }
    if (!project_side(ctx, d->src, src_cols, n, cs, &d->opts.compare, ss->first_data_row, psrc) ||
        !project_side(ctx, d->tgt, tgt_cols, n, cs, &d->opts.compare, ts->first_data_row, ptgt)) {
        return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "diff: cannot project the compared columns");
    }

    d->projected = 1;
    d->src0 = d->src;
    d->tgt0 = d->tgt;
    d->ss0 = ss;
    d->ts0 = ts;
    d->src = psrc;
    d->tgt = ptgt;
    d->ss = cs;
    d->ts = cts;
    return IBHA_CSVD_OK;
}

/* ------------------------------------------------ anchoring deleted rows -- */

/*
 * Assigns every deleted source row to a bucket, then threads the buckets into
 * lists in source order so that consecutive deletions stay grouped.
 *
 * Bucket 0 is "before the first target row" and bucket t + 1 is "after target
 * row t". Putting every deletion in bucket n_tgt is exactly
 * IBHA_CSVD_DELETED_END, so the same walk serves both placements and there is no
 * second code path to keep correct.
 *
 * The anchors are written into del_next and then read back out of it as the
 * lists are built. That is safe because the backward pass reads slot si before
 * it writes slot si, and it saves an array the size of the source side.
 */
static void anchor_deletions(ibha_csvd_diff *d) {
    int at_end = (d->opts.deleted_placement == IBHA_CSVD_DELETED_END);
    uint32_t last = at_end ? d->n_tgt : 0;

    for (uint32_t si = 0; si < d->n_src; si++) {
        if (d->s2t[si] != NO_ROW) {
            if (!at_end) last = d->s2t[si] + 1;
        } else {
            d->del_next[si] = last;
        }
    }
    for (uint32_t si = d->n_src; si-- > 0;) {
        if (d->s2t[si] != NO_ROW) continue;
        uint32_t bucket = d->del_next[si];
        d->del_next[si] = d->del_head[bucket];
        d->del_head[bucket] = si;
    }
}

/* ------------------------------------------------------------------- run -- */

ibha_csvd_diff *ibha_csvd_diff_run(ibha_csvd_ctx *ctx, const ibha_csvd_table *src,
                                   const ibha_csvd_schema *src_schema, const ibha_csvd_table *tgt,
                                   const ibha_csvd_schema *tgt_schema,
                                   const ibha_csvd_diff_opts *opts) {
    if (!ctx) return NULL;
    if (ctx->status != IBHA_CSVD_OK) return NULL;
    if (!src || !src_schema || !tgt || !tgt_schema) {
        ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG, "diff: both a table and a schema are required "
                                                 "for each side");
        return NULL;
    }

    ibha_csvd_diff *d = (ibha_csvd_diff *)ibha_arena_calloc(&ctx->arena, sizeof(*d));
    if (!d) {
        ibha_err(ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the diff");
        return NULL;
    }
    d->ctx = ctx;
    d->src = src;
    d->tgt = tgt;
    d->ss = src_schema;
    d->ts = tgt_schema;
    if (opts) {
        d->opts = *opts;
    } else {
        ibha_csvd_diff_opts_init(&d->opts);
    }
    ibha_compare_opts_resolve(&d->opts.compare);

    if (d->opts.similarity_k == 0) d->opts.similarity_k = 1;
    if (d->opts.similarity_k > 4096) d->opts.similarity_k = 4096;
    if (d->opts.similarity_percent > 100) d->opts.similarity_percent = 100;
    if (d->opts.max_pair_work == 0) d->opts.max_pair_work = 4000000;
    if (d->opts.deleted_placement != IBHA_CSVD_DELETED_END) {
        d->opts.deleted_placement = IBHA_CSVD_DELETED_ANCHORED;
    }
    /*
     * Spec 6.7. A relational source with no ORDER BY has no meaningful row
     * order, so the longest increasing subsequence over source indices would
     * report an arbitrary and unreproducible set of rows as moved, and anchoring
     * a deleted row next to its former neighbours would be meaningless. Both
     * degrade, and the degradation is reported rather than applied silently.
     */
    if (!d->opts.source_ordered) {
        d->stats.moves_forced_off = d->opts.detect_moves ? 1 : 0;
        d->opts.detect_moves = 0;
        d->opts.deleted_placement = IBHA_CSVD_DELETED_END;
    }

    /* The column plan runs first, because when it is in force everything after it
     * works on the projected tables and the compared schema rather than on what
     * the caller handed in. */
    if (plan_columns(d) != IBHA_CSVD_OK) return NULL;
    if (check_sides(ctx, d->src, d->ss, d->tgt, d->ts, &d->opts) != IBHA_CSVD_OK) return NULL;

    d->n_columns = d->ss->n_columns;
    d->s_first = d->ss->first_data_row;
    d->t_first = d->ts->first_data_row;
    src = d->src;
    tgt = d->tgt;
    d->n_src = src->n_rows > d->s_first ? src->n_rows - d->s_first : 0;
    d->n_tgt = tgt->n_rows > d->t_first ? tgt->n_rows - d->t_first : 0;

    /* One allocation per array, all sized by row count. This is the whole of the
     * diff's own memory: no report, no per cell state, nothing proportional to
     * the size of the difference. */
    d->t2s = (uint32_t *)ibha_arena_alloc_large(&ctx->arena, (size_t)(d->n_tgt + 1) * 4);
    d->s2t = (uint32_t *)ibha_arena_alloc_large(&ctx->arena, (size_t)(d->n_src + 1) * 4);
    d->t_moved = (uint8_t *)ibha_arena_calloc(&ctx->arena, d->n_tgt + 1);
    d->t_move_dist = (int32_t *)ibha_arena_calloc(&ctx->arena, (size_t)(d->n_tgt + 1) * 4);
    d->del_head = (uint32_t *)ibha_arena_alloc_large(&ctx->arena, (size_t)(d->n_tgt + 2) * 4);
    d->del_next = (uint32_t *)ibha_arena_alloc_large(&ctx->arena, (size_t)(d->n_src + 1) * 4);
    if (!d->t2s || !d->s2t || !d->t_moved || !d->t_move_dist || !d->del_head || !d->del_next) {
        ibha_err(ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the match arrays");
        return NULL;
    }
    for (uint32_t i = 0; i < d->n_tgt; i++) d->t2s[i] = NO_ROW;
    for (uint32_t i = 0; i < d->n_src; i++) d->s2t[i] = NO_ROW;
    for (uint32_t i = 0; i <= d->n_tgt; i++) d->del_head[i] = NO_ROW;

    if (ibha_match(d) != IBHA_CSVD_OK) return NULL;
    anchor_deletions(d);
    if (ibha_validate_plan(d) != IBHA_CSVD_OK) return NULL;

    /*
     * Row level counts are final here, before a single cell has been read,
     * because the digests already answered every question that does not need
     * one. Cell counts are the cursor's job.
     */
    for (uint32_t ti = 0; ti < d->n_tgt; ti++) {
        uint32_t si = d->t2s[ti];
        if (si == NO_ROW) {
            d->stats.rows_added++;
            continue;
        }
        if (src->row_full_hash[d->s_first + si] == tgt->row_full_hash[d->t_first + ti]) {
            d->stats.rows_unchanged++;
        } else {
            d->stats.rows_modified++;
        }
        if (d->t_moved[ti]) d->stats.rows_moved++;
    }
    for (uint32_t si = 0; si < d->n_src; si++) {
        if (d->s2t[si] == NO_ROW) d->stats.rows_deleted++;
    }
    d->stats.report_rows = d->n_tgt + d->stats.rows_deleted;
    return d;
}

const ibha_csvd_diff_stats *ibha_csvd_diff_stats_of(const ibha_csvd_diff *d) {
    return d ? &d->stats : NULL;
}

/*
 * The projected tables, not the parsed ones. See the header: under the column
 * policy of spec 6.6 these differ, and a consumer decoding cells out of the
 * parsed table then reads the wrong column.
 */
const ibha_csvd_table *ibha_csvd_diff_table(const ibha_csvd_diff *d, ibha_csvd_side side) {
    if (!d) return NULL;
    if (side == IBHA_CSVD_SIDE_SOURCE) return d->src;
    if (side == IBHA_CSVD_SIDE_TARGET) return d->tgt;
    return NULL;
}

const ibha_csvd_schema *ibha_csvd_diff_schema(const ibha_csvd_diff *d, ibha_csvd_side side) {
    if (!d) return NULL;
    if (side == IBHA_CSVD_SIDE_SOURCE) return d->ss;
    if (side == IBHA_CSVD_SIDE_TARGET) return d->ts;
    return NULL;
}

uint32_t ibha_csvd_diff_columns(const ibha_csvd_diff *d) { return d ? d->n_columns : 0u; }

/* ---------------------------------------------------------------- cursor -- */

struct ibha_csvd_cursor {
    ibha_csvd_diff *d;
    uint32_t bucket; /* next target row to emit, and the bucket drained before it */
    uint32_t del;    /* next deleted source row in that bucket, or NO_ROW */
    int done;
    int live; /* a row has been produced and not yet superseded */
    ibha_csvd_row row;
    uint8_t *cell_flags;
};

ibha_csvd_cursor *ibha_csvd_cursor_open(ibha_csvd_diff *d) {
    if (!d) return NULL;
    if (d->ctx->status != IBHA_CSVD_OK) return NULL;

    ibha_csvd_cursor *c = (ibha_csvd_cursor *)ibha_arena_calloc(&d->ctx->arena, sizeof(*c));
    if (!c) {
        ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the cursor");
        return NULL;
    }
    c->d = d;
    c->cell_flags = (uint8_t *)ibha_arena_calloc(&d->ctx->arena, d->n_columns + 1);
    if (!c->cell_flags) {
        ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the cursor row");
        return NULL;
    }
    c->row.cell_flags = c->cell_flags;
    c->row.n_columns = d->n_columns;
    ibha_csvd_cursor_reset(c);
    return c;
}

void ibha_csvd_cursor_reset(ibha_csvd_cursor *c) {
    if (!c) return;
    c->bucket = 0;
    c->del = c->d->del_head[0];
    c->done = 0;
    c->live = 0;
}

/*
 * Fills the cell flags for a matched pair.
 *
 * Two walks are possible and only one of them is ever taken. A modified row is
 * walked to find which cells changed. An unchanged row is walked only when
 * count_suppressed is on *and* the two raw digests disagree, which is exactly
 * the case "these rows are equal only because normalization suppressed
 * something". Spec 5.3 requires that be counted rather than hidden, and the raw
 * digest is what makes it detectable without walking every unchanged row.
 */
static void fill_cells(ibha_csvd_cursor *c, uint32_t srow, uint32_t trow, int modified) {
    ibha_csvd_diff *d = c->d;
    int raw_differs = d->src->row_raw_hash[srow] != d->tgt->row_raw_hash[trow];
    int want_suppressed = d->opts.count_suppressed && raw_differs;

    for (uint32_t i = 0; i < d->n_columns; i++) c->cell_flags[i] = 0;
    c->row.n_changed_cells = 0;
    c->row.n_suppressed_cells = 0;
    if (!modified && !want_suppressed) return;

    uint32_t sbase = d->src->row_first_field[srow];
    uint32_t tbase = d->tgt->row_first_field[trow];

    for (uint32_t col = 0; col < d->n_columns; col++) {
        if (modified && !ibha_cell_equal(d, srow, trow, col)) {
            c->cell_flags[col] |= IBHA_CSVD_CELL_CHANGED;
            c->row.n_changed_cells++;
            continue;
        }
        if (want_suppressed &&
            ibha_csvd_field_cmp(d->src, sbase + col, d->tgt, tbase + col) != 0) {
            c->cell_flags[col] |= IBHA_CSVD_CELL_SUPPRESSED;
            c->row.n_suppressed_cells++;
        }
    }
    d->stats.cells_changed += c->row.n_changed_cells;
    d->stats.cells_suppressed += c->row.n_suppressed_cells;
}

static void emit_target(ibha_csvd_cursor *c, uint32_t ti) {
    ibha_csvd_diff *d = c->d;
    uint32_t si = d->t2s[ti];
    uint32_t trow = d->t_first + ti;

    c->row.target_row = trow;
    c->row.moved = 0;
    c->row.move_distance = 0;
    c->row.n_changed_cells = 0;
    c->row.n_suppressed_cells = 0;
    c->row.n_findings = 0;

    if (si == NO_ROW) {
        c->row.kind = IBHA_CSVD_ROW_ADDED;
        c->row.source_row = NO_ROW;
        for (uint32_t i = 0; i < d->n_columns; i++) c->cell_flags[i] = 0;
    } else {
        uint32_t srow = d->s_first + si;
        int modified = d->src->row_full_hash[srow] != d->tgt->row_full_hash[trow];

        c->row.kind = (uint8_t)(modified ? IBHA_CSVD_ROW_MODIFIED : IBHA_CSVD_ROW_UNCHANGED);
        c->row.source_row = srow;
        c->row.moved = d->t_moved[ti];
        c->row.move_distance = d->t_moved[ti] ? d->t_move_dist[ti] : 0;
        fill_cells(c, srow, trow, modified);
    }

    /* Findings are about the values the row carries, which for anything with a
     * target is the uploaded file's values. This is the one thing that makes the
     * cursor read the cells of an unchanged row: the digest settled whether they
     * changed, and cannot settle whether they satisfy the schema. */
    c->row.n_findings = ibha_validate_row(d, d->tgt, trow, c->cell_flags);
}

static void emit_deleted(ibha_csvd_cursor *c, uint32_t si) {
    ibha_csvd_diff *d = c->d;
    uint32_t srow = d->s_first + si;

    c->row.kind = IBHA_CSVD_ROW_DELETED;
    c->row.source_row = srow;
    c->row.target_row = NO_ROW;
    c->row.moved = 0;
    c->row.move_distance = 0;
    c->row.n_changed_cells = 0;
    c->row.n_suppressed_cells = 0;
    for (uint32_t i = 0; i < d->n_columns; i++) c->cell_flags[i] = 0;
    /* A deleted row has no target values, so its findings are about the source
     * values, which is the only thing it reports. */
    c->row.n_findings = ibha_validate_row(d, d->src, srow, c->cell_flags);
}

int ibha_csvd_cursor_next(ibha_csvd_cursor *c) {
    if (!c) return -1;
    if (c->d->ctx->status != IBHA_CSVD_OK) return -1;
    if (c->done) return 0;

    /* Drain the bucket that sits before the next target row, then the row
     * itself, then advance to the bucket that follows it. */
    if (c->del != NO_ROW) {
        uint32_t si = c->del;
        c->del = c->d->del_next[si];
        emit_deleted(c, si);
        c->live = 1;
        return 1;
    }
    if (c->bucket < c->d->n_tgt) {
        uint32_t ti = c->bucket++;
        c->del = c->d->del_head[c->bucket];
        emit_target(c, ti);
        c->live = 1;
        return 1;
    }
    c->done = 1;
    c->live = 0;
    return 0;
}

const ibha_csvd_row *ibha_csvd_cursor_row(const ibha_csvd_cursor *c) {
    if (!c || !c->live) return NULL;
    return &c->row;
}
