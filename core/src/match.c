/*
 * match.c - row matching, duplicate key detection and move detection.
 *
 * Three algorithms live here and each exists to avoid a specific wrong answer.
 *
 * **Keyed matching, spec 6.1.** An open addressing table over row_key_hash, then
 * one walk of the target rows. This is what makes row reordering a non issue: a
 * row that moved is found by key wherever it now sits. A key hash collision
 * costs a key comparison, never a wrong pairing, because every hash match is
 * verified against the key bytes.
 *
 * **Move detection by longest increasing subsequence, spec 6.2.** Marking every
 * row whose position changed as moved would report all 300,000 rows as moved
 * when one row is dragged to the top. The LIS is the set of rows that are in
 * place; only the rest moved, and one row dragged to the top reports exactly one
 * moved row.
 *
 * **The all keys case, spec 6.4.** When every column is part of the key, a
 * modified row cannot be found by key, because changing any cell changes the
 * key. Naively that is one deleted plus one added, which is technically correct
 * and practically unhelpful. So: exact multiset matching on the full row digest
 * first, then bounded similarity pairing over whatever is left.
 *
 * Everything is data relative: row 0 is the first data row on its side, and the
 * header rows are outside all of it.
 */
#include "internal.h"

#define NO_ROW IBHA_CSVD_NO_ROW

/* Enough entries for the candidate index of spec 6.4 before it is considered
 * pathological. Past this the signature width shrinks, and past that pairing is
 * truncated with a diagnostic rather than being allowed to dominate the arena. */
#define SIG_MAX_ENTRIES (1u << 20)

/* --------------------------------------------------------------- helpers -- */

static uint32_t pow2_at_least(uint64_t n) {
    uint32_t cap = 16;
    while ((uint64_t)cap < n) {
        if (cap > 0x40000000u) return 0x80000000u;
        cap *= 2;
    }
    return cap;
}

/* Normalized equality of one column between two rows, which may be on the same
 * side or on opposite sides. The declared type comes from the source schema,
 * which spec 13.8 makes authoritative for every piece of schema metadata. */
static int col_equal(const ibha_csvd_diff *d, const ibha_csvd_table *ta, uint32_t ra,
                     const ibha_csvd_table *tb, uint32_t rb, uint32_t col) {
    uint32_t fa = ta->row_first_field[ra] + col;
    uint32_t fb = tb->row_first_field[rb] + col;
    uint8_t sa[IBHA_NORM_SCRATCH], sb[IBHA_NORM_SCRATCH];
    ibha_norm na, nb;
    uint8_t type = d->ss->col_type ? d->ss->col_type[col] : (uint8_t)IBHA_CSVD_TYPE_UNKNOWN;

    (void)ibha_normalize(ta, fa, type, &d->opts.compare, sa, &na);
    (void)ibha_normalize(tb, fb, type, &d->opts.compare, sb, &nb);
    return ibha_norm_cmp(&na, &nb, ta->quote) == 0;
}

int ibha_cell_equal(const ibha_csvd_diff *d, uint32_t srow, uint32_t trow, uint32_t col) {
    return col_equal(d, d->src, srow, d->tgt, trow, col);
}

/* The normalized hash of one cell, which is the same value the row digests were
 * folded from. Used to build the spec 6.4 candidate signatures. */
static uint64_t col_hash(const ibha_csvd_diff *d, const ibha_csvd_table *t, uint32_t row,
                         uint32_t col) {
    uint32_t f = t->row_first_field[row] + col;
    uint8_t scratch[IBHA_NORM_SCRATCH];
    ibha_norm nv;
    uint8_t type = d->ss->col_type ? d->ss->col_type[col] : (uint8_t)IBHA_CSVD_TYPE_UNKNOWN;
    (void)ibha_normalize(t, f, type, &d->opts.compare, scratch, &nv);
    return ibha_norm_hash(&nv, t->quote);
}

static int keys_equal(const ibha_csvd_diff *d, const ibha_csvd_table *ta, uint32_t ra,
                      const ibha_csvd_table *tb, uint32_t rb) {
    for (uint32_t c = 0; c < d->n_columns; c++) {
        if (!(d->ss->col_flags[c] & IBHA_CSVD_COL_KEY)) continue;
        if (!col_equal(d, ta, ra, tb, rb, c)) return 0;
    }
    return 1;
}

/*
 * Renders a row's key for an error message. "duplicate key" without the key and
 * the row numbers is useless in a 90,000 row file, which is exactly why spec
 * 13.9 spells out what the message has to contain. This is a rendering boundary,
 * so materializing the values here is the sanctioned use of field_copy.
 */
#define KEY_MSG_CAP 120

/* At most `limit` bytes of a field's logical value, collapsing "" as it goes.
 * field_copy writes nothing at all when the value does not fit, which is right
 * for an export boundary and wrong for a message that wants a readable prefix. */
static uint32_t logical_prefix(const ibha_csvd_table *t, uint32_t f, uint8_t *dst, uint32_t limit) {
    const uint8_t *p = t->bytes + t->field_off[f];
    uint32_t len = t->field_len[f];
    int esc = t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE;
    uint32_t w = 0;

    for (uint32_t i = 0; i < len && w < limit; i++) {
        if (esc && p[i] == t->quote && i + 1 < len && p[i + 1] == t->quote) i++;
        dst[w++] = p[i];
    }
    return w;
}

static void key_str(const ibha_csvd_diff *d, const ibha_csvd_table *t, uint32_t row, char *dst,
                    size_t cap) {
    size_t w = 0;
    int first = 1;

    for (uint32_t c = 0; c < d->n_columns && w + 8 < cap; c++) {
        if (!(d->ss->col_flags[c] & IBHA_CSVD_COL_KEY)) continue;
        if (!first) {
            dst[w++] = ',';
            dst[w++] = ' ';
        }
        first = 0;

        uint8_t buf[40];
        uint32_t f = t->row_first_field[row] + c;
        uint32_t n = logical_prefix(t, f, buf, (uint32_t)sizeof(buf));
        for (uint32_t i = 0; i < n && w + 5 < cap; i++) {
            uint8_t ch = buf[i];
            dst[w++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
        }
    }
    dst[w < cap ? w : cap - 1] = '\0';
}

/* ------------------------------------------------------------ bucket map -- */
/*
 * Open addressing with linear probing, per spec 3.1. Slots hold the data
 * relative row index plus one, so a zeroed arena block is an empty table and no
 * separate occupancy array is needed.
 *
 * Duplicates matter differently on the two paths. With a declared key they are a
 * hard error (spec 13.9). With no declared key every column is the key, so
 * identical rows are legitimate and their multiplicity is the thing spec 6.4
 * stage 1 has to preserve; there the table simply holds them all and lookups
 * consume one at a time.
 */
typedef struct {
    uint32_t *slot;
    uint32_t mask;
} bmap;

static int bmap_init(ibha_csvd_diff *d, bmap *m, uint32_t n) {
    uint32_t cap = pow2_at_least((uint64_t)n * 2u + 1u);
    m->slot = (uint32_t *)ibha_arena_calloc(&d->ctx->arena, (size_t)cap * 4);
    m->mask = cap - 1;
    return m->slot != NULL;
}

/* --------------------------------------------------- keyed matching, 6.1 -- */

static ibha_csvd_status dup_key_err(ibha_csvd_diff *d, const ibha_csvd_table *t, const char *side,
                                    uint32_t row_a, uint32_t row_b) {
    char key[KEY_MSG_CAP];
    key_str(d, t, row_a, key, sizeof(key));
    return ibha_err(d->ctx, IBHA_CSVD_ERR_DUPLICATE_KEY,
                    "duplicate key (%s) in the %s at rows %llu and %llu", key, side,
                    (uint64_t)row_a + 1, (uint64_t)row_b + 1);
}

static ibha_csvd_status key_map_build(ibha_csvd_diff *d, const ibha_csvd_table *t, uint32_t first,
                                      uint32_t n, const char *side, bmap *out) {
    if (!bmap_init(d, out, n)) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the %s key index", side);
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t row = first + i;
        uint64_t h = t->row_key_hash[row];
        uint32_t idx = (uint32_t)h & out->mask;

        for (;;) {
            uint32_t cur = out->slot[idx];
            if (cur == 0) {
                out->slot[idx] = i + 1;
                break;
            }
            uint32_t other = first + (cur - 1);
            /* The hash test is the filter and the key comparison is the answer:
             * a collision costs one comparison, never a wrong verdict. */
            if (t->row_key_hash[other] == h && keys_equal(d, t, other, t, row)) {
                return dup_key_err(d, t, side, other, row);
            }
            idx = (idx + 1) & out->mask;
        }
    }
    return IBHA_CSVD_OK;
}

static ibha_csvd_status match_keyed(ibha_csvd_diff *d) {
    bmap smap, tmap;
    ibha_csvd_status st = key_map_build(d, d->src, d->s_first, d->n_src, "source file", &smap);
    if (st != IBHA_CSVD_OK) return st;
    /* Checked on both sides, per spec 13.9. A duplicate that never matches a
     * source key would otherwise go unnoticed until it produced two added rows
     * that look like a legitimate pair of new records. */
    st = key_map_build(d, d->tgt, d->t_first, d->n_tgt, "uploaded file", &tmap);
    if (st != IBHA_CSVD_OK) return st;

    for (uint32_t ti = 0; ti < d->n_tgt; ti++) {
        uint32_t trow = d->t_first + ti;
        uint64_t h = d->tgt->row_key_hash[trow];
        uint32_t idx = (uint32_t)h & smap.mask;

        for (;;) {
            uint32_t cur = smap.slot[idx];
            if (cur == 0) break; /* no matching key: added */
            uint32_t si = cur - 1;
            uint32_t srow = d->s_first + si;
            if (d->src->row_key_hash[srow] == h && keys_equal(d, d->src, srow, d->tgt, trow)) {
                d->t2s[ti] = si;
                d->s2t[si] = ti;
                break;
            }
            idx = (idx + 1) & smap.mask;
        }
    }
    return IBHA_CSVD_OK;
}

/* ------------------------------------------------ the all keys case, 6.4 -- */

/* Stage 1: exact multiset match on the full row digest, so three identical rows
 * on each side match three to three. */
static ibha_csvd_status all_keys_exact(ibha_csvd_diff *d, uint8_t *consumed) {
    bmap m;
    if (!bmap_init(d, &m, d->n_src)) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the row index");
    }
    for (uint32_t si = 0; si < d->n_src; si++) {
        uint64_t h = d->src->row_full_hash[d->s_first + si];
        uint32_t idx = (uint32_t)h & m.mask;
        while (m.slot[idx] != 0) idx = (idx + 1) & m.mask;
        m.slot[idx] = si + 1;
    }

    for (uint32_t ti = 0; ti < d->n_tgt; ti++) {
        uint64_t h = d->tgt->row_full_hash[d->t_first + ti];
        uint32_t idx = (uint32_t)h & m.mask;
        for (;;) {
            uint32_t cur = m.slot[idx];
            if (cur == 0) break;
            uint32_t si = cur - 1;
            /* Rows with equal hashes were inserted in ascending order from the
             * same base slot, so the first unconsumed one is the lowest indexed
             * one and the pairing is reproducible. */
            if (!consumed[si] && d->src->row_full_hash[d->s_first + si] == h) {
                consumed[si] = 1;
                d->t2s[ti] = si;
                d->s2t[si] = ti;
                break;
            }
            idx = (idx + 1) & m.mask;
        }
    }
    return IBHA_CSVD_OK;
}

/*
 * Stage 2: similarity pairing over the leftovers.
 *
 * Unmatched rows are normally a small fraction, so an O(n) pass with bounded
 * constants replaces the O(n^2) a full pairwise search would cost. Each
 * unmatched source row is indexed under the hash of a few of its individual
 * column values; each unmatched target row gathers candidates from those
 * buckets, scores at most similarity_k of them by the fraction of matching
 * cells, and takes the best above the threshold.
 *
 * The threshold is an integer percentage. Nothing here is floating point,
 * because spec 3.2 requires the wasm32 and native builds to produce byte
 * identical output and a fraction comparison is the obvious place for them to
 * stop doing so.
 */
static ibha_csvd_status all_keys_similar(ibha_csvd_diff *d, uint8_t *consumed) {
    uint32_t n_left_src = 0;
    for (uint32_t si = 0; si < d->n_src; si++) {
        if (!consumed[si]) n_left_src++;
    }
    if (n_left_src == 0 || d->n_columns == 0) return IBHA_CSVD_OK;

    uint32_t sig_cols = d->n_columns < IBHA_SIG_COLUMNS ? d->n_columns : IBHA_SIG_COLUMNS;
    while (sig_cols > 1 && (uint64_t)n_left_src * sig_cols > SIG_MAX_ENTRIES) sig_cols--;
    if ((uint64_t)n_left_src * sig_cols > SIG_MAX_ENTRIES) {
        /* Hundreds of thousands of unmatched rows with no discriminating column.
         * Spec 6.4 asks for a diagnostic rather than a hang, and leaving them as
         * added and deleted is the honest answer. */
        d->stats.pairing_truncated = 1;
        return IBHA_CSVD_OK;
    }

    uint32_t cap = pow2_at_least((uint64_t)n_left_src * sig_cols * 2u + 1u);
    uint64_t *sig_h = (uint64_t *)ibha_arena_alloc_large(&d->ctx->arena, (size_t)cap * 8);
    uint32_t *sig_r = (uint32_t *)ibha_arena_calloc(&d->ctx->arena, (size_t)cap * 4);
    uint32_t *seen = (uint32_t *)ibha_arena_calloc(&d->ctx->arena, (size_t)d->n_src * 4);
    uint32_t *cand = (uint32_t *)ibha_arena_alloc(&d->ctx->arena,
                                                  (size_t)d->opts.similarity_k * 4);
    if (!sig_h || !sig_r || !seen || !cand) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the pairing index");
    }
    uint32_t mask = cap - 1;

    for (uint32_t si = 0; si < d->n_src; si++) {
        if (consumed[si]) continue;
        uint32_t srow = d->s_first + si;
        uint32_t used = 0;
        for (uint32_t c = 0; c < d->n_columns && used < sig_cols; c++) {
            uint32_t f = d->src->row_first_field[srow] + c;
            if (d->src->field_len[f] == 0) continue; /* an empty cell discriminates nothing */
            used++;
            uint64_t h = ibha_hash_mix(col_hash(d, d->src, srow, c), c);
            uint32_t idx = (uint32_t)h & mask;
            while (sig_r[idx] != 0) idx = (idx + 1) & mask;
            sig_h[idx] = h;
            sig_r[idx] = si + 1;
        }
    }

    uint64_t work = 0;
    for (uint32_t ti = 0; ti < d->n_tgt; ti++) {
        if (d->t2s[ti] != NO_ROW) continue;
        if (work > d->opts.max_pair_work) {
            d->stats.pairing_truncated = 1;
            break;
        }
        uint32_t trow = d->t_first + ti;
        uint32_t n_cand = 0;
        uint32_t stamp = ti + 1;

        uint32_t used = 0;
        for (uint32_t c = 0; c < d->n_columns && used < sig_cols && n_cand < d->opts.similarity_k;
             c++) {
            uint32_t f = d->tgt->row_first_field[trow] + c;
            if (d->tgt->field_len[f] == 0) continue;
            used++;
            uint64_t h = ibha_hash_mix(col_hash(d, d->tgt, trow, c), c);
            uint32_t idx = (uint32_t)h & mask;
            while (sig_r[idx] != 0 && n_cand < d->opts.similarity_k) {
                uint32_t si = sig_r[idx] - 1;
                if (sig_h[idx] == h && !consumed[si] && seen[si] != stamp) {
                    seen[si] = stamp;
                    cand[n_cand++] = si;
                }
                idx = (idx + 1) & mask;
            }
        }

        uint32_t best = NO_ROW, best_score = 0;
        for (uint32_t k = 0; k < n_cand; k++) {
            uint32_t si = cand[k];
            uint32_t hits = 0;
            for (uint32_t c = 0; c < d->n_columns; c++) {
                if (ibha_cell_equal(d, d->s_first + si, trow, c)) hits++;
            }
            work += d->n_columns;
            uint32_t score = hits * 100u / d->n_columns;
            /* Ties go to the lowest source row, so the report does not depend on
             * the order the candidate buckets happened to be walked in. */
            if (best == NO_ROW || score > best_score || (score == best_score && si < best)) {
                best_score = score;
                best = si;
            }
        }

        if (best != NO_ROW && best_score >= d->opts.similarity_percent) {
            consumed[best] = 1;
            d->t2s[ti] = best;
            d->s2t[best] = ti;
            d->stats.paired_by_similarity++;
        }
    }
    return IBHA_CSVD_OK;
}

static ibha_csvd_status match_all_keys(ibha_csvd_diff *d) {
    d->stats.all_keys = 1;
    uint8_t *consumed = (uint8_t *)ibha_arena_calloc(&d->ctx->arena, d->n_src ? d->n_src : 1);
    if (!consumed) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the match state");
    }
    ibha_csvd_status st = all_keys_exact(d, consumed);
    if (st != IBHA_CSVD_OK) return st;
    return all_keys_similar(d, consumed);
}

/* --------------------------------------------- move detection, spec 6.2 -- */

static ibha_csvd_status detect_moves(ibha_csvd_diff *d) {
    uint32_t m = 0;
    for (uint32_t ti = 0; ti < d->n_tgt; ti++) {
        if (d->t2s[ti] != NO_ROW) m++;
    }
    if (m == 0) return IBHA_CSVD_OK;

    /*
     * moveDistance is the shift in rank among the matched rows rather than the
     * difference of raw row numbers, so that rows either side of a deletion are
     * not all reported as having drifted by one. Written in two passes: the
     * source pass subtracts the source rank, the target pass adds the target
     * rank.
     */
    uint32_t rank = 0;
    for (uint32_t si = 0; si < d->n_src; si++) {
        if (d->s2t[si] == NO_ROW) continue;
        d->t_move_dist[d->s2t[si]] = -(int32_t)rank;
        rank++;
    }
    rank = 0;
    for (uint32_t ti = 0; ti < d->n_tgt; ti++) {
        if (d->t2s[ti] == NO_ROW) continue;
        d->t_move_dist[ti] += (int32_t)rank;
        rank++;
    }

    if (!d->opts.detect_moves || !d->opts.source_ordered) return IBHA_CSVD_OK;

    uint32_t *jt = (uint32_t *)ibha_arena_alloc_large(&d->ctx->arena, (size_t)m * 4);
    uint32_t *tails = (uint32_t *)ibha_arena_alloc_large(&d->ctx->arena, (size_t)m * 4);
    uint32_t *prev = (uint32_t *)ibha_arena_alloc_large(&d->ctx->arena, (size_t)m * 4);
    if (!jt || !tails || !prev) {
        return ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "diff: cannot allocate the move detector");
    }

    uint32_t j = 0;
    for (uint32_t ti = 0; ti < d->n_tgt; ti++) {
        if (d->t2s[ti] == NO_ROW) continue;
        jt[j++] = ti;
        d->t_moved[ti] = 1; /* cleared again for everything on the subsequence */
    }

    /* Patience style: tails[k] is the position, in target order, of the smallest
     * source index that can end an increasing subsequence of length k + 1.
     * O(n log n), a few milliseconds at 300,000 rows. */
    uint32_t len = 0;
    for (j = 0; j < m; j++) {
        uint32_t v = d->t2s[jt[j]];
        uint32_t lo = 0, hi = len;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (d->t2s[jt[tails[mid]]] < v) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        prev[j] = lo ? tails[lo - 1] : NO_ROW;
        tails[lo] = j;
        if (lo == len) len++;
    }

    for (uint32_t k = tails[len - 1];; k = prev[k]) {
        d->t_moved[jt[k]] = 0;
        if (prev[k] == NO_ROW) break;
    }
    return IBHA_CSVD_OK;
}

/* ----------------------------------------------------------------- entry -- */

ibha_csvd_status ibha_match(ibha_csvd_diff *d) {
    ibha_csvd_status st = d->ss->n_key_columns > 0 ? match_keyed(d) : match_all_keys(d);
    if (st != IBHA_CSVD_OK) return st;
    return detect_moves(d);
}
