/*
 * segment.c - the intra cell diff of spec 7.
 *
 * The case it exists for: "Accident violation code" becoming "Accident Violation
 * code(s)". Highlighting the whole cell there loses exactly the information the
 * reviewer needs, which is that one letter changed case and two characters were
 * added.
 *
 * Three properties keep it cheap, and all three are structural rather than
 * hopeful:
 *
 * **Nothing is computed until it is asked for.** There is no pass over the diff
 * that fills cell segments in. The view calls this for the cells in its visible
 * window and a 300,000 row diff computes 30 cell diffs, not 300,000.
 *
 * **It is capped twice.** A pair longer than max_bytes is reported as wholly
 * replaced without running Myers at all, and a pair needing more than
 * SEG_MAX_EDITS edits gives up and reports the same thing. Myers is O(ND): fine
 * for short strings, quadratic-ish for long dissimilar ones, and a cell that
 * needs sixty-four edits produces a highlight that is noise anyway.
 *
 * **Its memory is one scratch block owned by the diff**, allocated on first use,
 * sized by the cap and reused by every later call. Nothing accumulates per cell.
 *
 * On memoization: spec 7 says results are memoized in the arena. They are not
 * memoized here, and that is deliberate. A memo table keyed by cell is state that
 * grows with the number of cells looked at, which is exactly what spec 13.3
 * forbids the streaming path from carrying. The natural owner is the random
 * access consumer of spec 4.2, which already retains an index and knows which
 * cells its viewport keeps asking about. See the Phase 3 handoff.
 */
#include "internal.h"

/* A pair needing more edits than this reports as wholly replaced. The trace is
 * (SEG_MAX_EDITS + 1) rows of (2 * SEG_MAX_EDITS + 3) int32, so this number is
 * what sets the scratch block's fixed part: 64 costs 34 KB, 128 would cost
 * 133 KB, and the extra detail buys nothing a reviewer can read. */
#define SEG_MAX_EDITS 64
#define SEG_TRACE_ROW (2 * SEG_MAX_EDITS + 3)
#define SEG_V_ORIGIN (SEG_MAX_EDITS + 1)

/* Hard ceiling on the caller's max_bytes, so an absurd request cannot turn the
 * scratch block into the largest thing in the arena. */
#define SEG_MAX_CAP (256u * 1024u)

/* ---------------------------------------------------------------- scratch -- */

typedef struct {
    uint32_t cap; /* logical bytes per side */
    uint8_t *a;
    uint8_t *b;
    /* Element tables: start and length interleaved, one pair per element. Two
     * sets, because the character refinement of word-then-character runs while
     * the word level tables are still being walked. */
    uint32_t *ea;
    uint32_t *eb;
    uint32_t *ea2;
    uint32_t *eb2;
    uint8_t *ops;  /* the edit script, one op per element */
    uint8_t *ops2; /* the refinement's */
    int32_t *v;
    int32_t *trace;
} seg_mem;

static seg_mem *seg_scratch(ibha_csvd_diff *d, uint32_t cap) {
    seg_mem *m = (seg_mem *)d->seg_scratch;
    if (m && m->cap >= cap) return m;

    size_t n = cap;
    size_t elems = (n + 1) * 2 * sizeof(uint32_t);
    size_t need = sizeof(seg_mem) + 2 * n + 4 * elems + 2 * (2 * n + 2) +
                  (size_t)SEG_TRACE_ROW * sizeof(int32_t) +
                  (size_t)(SEG_MAX_EDITS + 1) * SEG_TRACE_ROW * sizeof(int32_t) + 64;

    uint8_t *block = (uint8_t *)ibha_arena_alloc_large(&d->ctx->arena, need);
    if (!block) {
        ibha_err(d->ctx, IBHA_CSVD_ERR_OOM, "segments: cannot allocate %zu bytes of scratch", need);
        return NULL;
    }

    /* Carved in descending alignment order, so no padding is needed between the
     * int32 tables and the byte tables. */
    m = (seg_mem *)block;
    uint8_t *p = block + sizeof(seg_mem);
    m->trace = (int32_t *)(void *)p;
    p += (size_t)(SEG_MAX_EDITS + 1) * SEG_TRACE_ROW * sizeof(int32_t);
    m->v = (int32_t *)(void *)p;
    p += (size_t)SEG_TRACE_ROW * sizeof(int32_t);
    m->ea = (uint32_t *)(void *)p;
    p += elems;
    m->eb = (uint32_t *)(void *)p;
    p += elems;
    m->ea2 = (uint32_t *)(void *)p;
    p += elems;
    m->eb2 = (uint32_t *)(void *)p;
    p += elems;
    m->a = p;
    p += n;
    m->b = p;
    p += n;
    /* An edit script is at most one op per element on each side. */
    m->ops = p;
    p += 2 * n + 2;
    m->ops2 = p;
    m->cap = cap;

    d->seg_scratch = m;
    d->seg_scratch_cap = need;
    return m;
}

/* ------------------------------------------------------------- emitting -- */

typedef struct {
    ibha_csvd_segment *out;
    uint32_t cap;
    uint32_t n; /* counts every segment, written or not */
    /* The previous segment is tracked here rather than read back out of out, so
     * that the returned count is the same whether the caller's buffer was big
     * enough or not. A count that depended on cap would make the size-then-fill
     * call pattern loop forever. */
    ibha_csvd_segment last;
    int have_last;
} seg_out;

/*
 * Appends one segment, merging it into the previous one when they are the same
 * op and abut. Merging is what turns the token level script "insert (, insert s,
 * insert )" into the single span a reviewer sees as "(s)".
 */
static void seg_push(seg_out *o, uint32_t op, uint32_t start, uint32_t len) {
    if (len == 0) return;

    if (o->have_last && o->last.op == op && o->last.start + o->last.len == start) {
        o->last.len += len;
        if (o->n - 1 < o->cap) o->out[o->n - 1].len = o->last.len;
        return;
    }
    if (o->n < o->cap) {
        o->out[o->n].op = op;
        o->out[o->n].start = start;
        o->out[o->n].len = len;
    }
    o->last.op = op;
    o->last.start = start;
    o->last.len = len;
    o->have_last = 1;
    o->n++;
}

/* ----------------------------------------------------------- tokenizing -- */

static int is_word_byte(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
           c >= 0x80;
}

/*
 * Splits into elements and returns how many. Word mode makes a maximal run of
 * word bytes one element and every other byte its own, which is spec 7's
 * "tokenize on whitespace and punctuation boundaries".
 *
 * Character mode makes each UTF-8 sequence one element rather than each byte, so
 * a multi byte character or an emoji is never split down the middle. Note that
 * this is code point granularity, not grapheme cluster granularity: a combining
 * mark is its own element. Clustering properly needs Unicode tables the engine
 * deliberately does not carry.
 */
static uint32_t tokenize(const uint8_t *p, uint32_t len, int word, uint32_t *e) {
    uint32_t n = 0, i = 0;
    while (i < len) {
        uint32_t start = i;
        if (word && is_word_byte(p[i])) {
            while (i < len && is_word_byte(p[i])) i++;
        } else if (word) {
            i++;
        } else {
            i++;
            while (i < len && (p[i] & 0xC0u) == 0x80u) i++;
        }
        e[2 * n] = start;
        e[2 * n + 1] = i - start;
        n++;
    }
    return n;
}

static int elem_eq(const uint8_t *a, const uint32_t *ea, uint32_t i, const uint8_t *b,
                   const uint32_t *eb, uint32_t j) {
    uint32_t la = ea[2 * i + 1], lb = eb[2 * j + 1];
    if (la != lb) return 0;
    return IBHA_MEMCMP(a + ea[2 * i], b + eb[2 * j], la) == 0;
}

/* ----------------------------------------------------------------- myers -- */

/*
 * Myers' O(ND) algorithm over elements, with the trace kept so the script can be
 * recovered. Returns the number of ops written into ops, or -1 when the pair
 * needs more than SEG_MAX_EDITS edits.
 *
 * ops is filled in *reverse* order by the backtrack and reversed by the caller's
 * forward walk, which is why nothing here needs a second array.
 */
#define OP_EQ 0
#define OP_DEL 1
#define OP_INS 2

static int myers(seg_mem *m, const uint8_t *a, const uint32_t *ea, int32_t na, const uint8_t *b,
                 const uint32_t *eb, int32_t nb, uint8_t *ops) {
    int32_t *v = m->v;
    int32_t d_found = -1;

    for (int32_t i = 0; i < SEG_TRACE_ROW; i++) v[i] = 0;

    for (int32_t d = 0; d <= SEG_MAX_EDITS; d++) {
        /* The snapshot is taken *before* the round, which is what makes the
         * backtrack's lookup of the predecessor well defined at d = 0. */
        int32_t *row = m->trace + (size_t)d * SEG_TRACE_ROW;
        for (int32_t i = 0; i < SEG_TRACE_ROW; i++) row[i] = v[i];

        for (int32_t k = -d; k <= d; k += 2) {
            int32_t x;
            if (k == -d || (k != d && v[SEG_V_ORIGIN + k - 1] < v[SEG_V_ORIGIN + k + 1])) {
                x = v[SEG_V_ORIGIN + k + 1];
            } else {
                x = v[SEG_V_ORIGIN + k - 1] + 1;
            }
            int32_t y = x - k;
            while (x < na && y < nb && elem_eq(a, ea, (uint32_t)x, b, eb, (uint32_t)y)) {
                x++;
                y++;
            }
            v[SEG_V_ORIGIN + k] = x;
            if (x >= na && y >= nb) {
                d_found = d;
                break;
            }
        }
        if (d_found >= 0) break;
    }
    if (d_found < 0) return -1;

    int32_t x = na, y = nb, n = 0;
    for (int32_t d = d_found; d >= 0; d--) {
        const int32_t *row = m->trace + (size_t)d * SEG_TRACE_ROW;
        int32_t k = x - y;
        int32_t prev_k;
        if (k == -d || (k != d && row[SEG_V_ORIGIN + k - 1] < row[SEG_V_ORIGIN + k + 1])) {
            prev_k = k + 1;
        } else {
            prev_k = k - 1;
        }
        int32_t prev_x = row[SEG_V_ORIGIN + prev_k];
        int32_t prev_y = prev_x - prev_k;

        while (x > prev_x && y > prev_y) {
            ops[n++] = OP_EQ;
            x--;
            y--;
        }
        if (d > 0) {
            if (x == prev_x) {
                ops[n++] = OP_INS;
            } else {
                ops[n++] = OP_DEL;
            }
        }
        x = prev_x;
        y = prev_y;
    }
    return n;
}

/* ------------------------------------------------------------ the walks -- */

/* Emits the script over one span pair at character granularity. Offsets are
 * added back so the segments index the whole value, not the span. */
static void refine(seg_mem *m, seg_out *o, const uint8_t *a, uint32_t aoff, uint32_t alen,
                   const uint8_t *b, uint32_t boff, uint32_t blen) {
    uint32_t na = tokenize(a + aoff, alen, 0, m->ea2);
    uint32_t nb = tokenize(b + boff, blen, 0, m->eb2);
    int n = myers(m, a + aoff, m->ea2, (int32_t)na, b + boff, m->eb2, (int32_t)nb, m->ops2);
    if (n < 0) {
        /* Too dissimilar to refine: the run is a plain replacement, which is what
         * the caller would have emitted without a refinement pass at all. */
        seg_push(o, IBHA_CSVD_SEG_DELETE, aoff, alen);
        seg_push(o, IBHA_CSVD_SEG_INSERT, boff, blen);
        return;
    }

    uint32_t ai = 0, bi = 0;
    for (int i = n - 1; i >= 0; i--) {
        switch (m->ops2[i]) {
            case OP_EQ:
                seg_push(o, IBHA_CSVD_SEG_EQUAL, aoff + m->ea2[2 * ai], m->ea2[2 * ai + 1]);
                ai++;
                bi++;
                break;
            case OP_DEL:
                seg_push(o, IBHA_CSVD_SEG_DELETE, aoff + m->ea2[2 * ai], m->ea2[2 * ai + 1]);
                ai++;
                break;
            default:
                seg_push(o, IBHA_CSVD_SEG_INSERT, boff + m->eb2[2 * bi], m->eb2[2 * bi + 1]);
                bi++;
                break;
        }
    }
}

/*
 * Walks the element level script forward, converting element indices to byte
 * ranges. Under word-then-character a maximal run of deletes and inserts is a
 * replacement, and it is handed to the character pass instead of being emitted
 * whole: that is what turns "code replaced by code(s)" into "code kept, (s)
 * added", which is what GitHub's intra line highlighting does and reads far
 * better than either level alone.
 */
/* Where element i begins, and where the elements end when i is past the last one.
 * A pure insertion run has no source element to take a position from, so it is
 * anchored at the boundary the elements around it define. */
static uint32_t elem_pos(const uint32_t *e, uint32_t n, uint32_t i, uint32_t base) {
    if (i < n) return e[2 * i];
    if (n == 0) return base;
    return e[2 * (n - 1)] + e[2 * (n - 1) + 1];
}

static void walk(seg_mem *m, seg_out *o, const uint8_t *a, const uint8_t *b, uint32_t na,
                 uint32_t nb, uint32_t base, const uint8_t *ops, int n, int refine_runs) {
    uint32_t ai = 0, bi = 0;

    int i = n - 1;
    while (i >= 0) {
        if (ops[i] == OP_EQ) {
            seg_push(o, IBHA_CSVD_SEG_EQUAL, m->ea[2 * ai], m->ea[2 * ai + 1]);
            ai++;
            bi++;
            i--;
            continue;
        }

        /* One maximal replacement run, in both values at once. */
        uint32_t astart = elem_pos(m->ea, na, ai, base);
        uint32_t bstart = elem_pos(m->eb, nb, bi, base);
        uint32_t abytes = 0, bbytes = 0;
        while (i >= 0 && ops[i] != OP_EQ) {
            if (ops[i] == OP_DEL) {
                abytes += m->ea[2 * ai + 1];
                ai++;
            } else {
                bbytes += m->eb[2 * bi + 1];
                bi++;
            }
            i--;
        }

        if (refine_runs && abytes && bbytes) {
            refine(m, o, a, astart, abytes, b, bstart, bbytes);
        } else {
            seg_push(o, IBHA_CSVD_SEG_DELETE, astart, abytes);
            seg_push(o, IBHA_CSVD_SEG_INSERT, bstart, bbytes);
        }
    }
}

/* ------------------------------------------------------------- the entry -- */

int ibha_csvd_cell_segments(ibha_csvd_diff *d, const ibha_csvd_row *row, uint32_t col,
                            ibha_csvd_cell_diff_mode mode, uint32_t max_bytes,
                            ibha_csvd_segment *out, uint32_t cap) {
    if (!d || !row) return IBHA_CSVD_ERR_INVALID_ARG;
    if (d->ctx->status != IBHA_CSVD_OK) return d->ctx->status;
    if (mode == IBHA_CSVD_CELLDIFF_NONE) return 0;
    if (mode != IBHA_CSVD_CELLDIFF_WORD && mode != IBHA_CSVD_CELLDIFF_CHARACTER &&
        mode != IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER) {
        return IBHA_CSVD_ERR_INVALID_ARG;
    }
    if (col >= d->n_columns) return IBHA_CSVD_ERR_INVALID_ARG;
    if (row->source_row == IBHA_CSVD_NO_ROW || row->target_row == IBHA_CSVD_NO_ROW) return 0;

    uint32_t fa = ibha_csvd_row_field(d->src, row->source_row, col);
    uint32_t fb = ibha_csvd_row_field(d->tgt, row->target_row, col);
    if (fa == 0xFFFFFFFFu || fb == 0xFFFFFFFFu) return 0;
    if (ibha_csvd_field_cmp(d->src, fa, d->tgt, fb) == 0) return 0;

    uint32_t la = ibha_csvd_field_logical_len(d->src, fa);
    uint32_t lb = ibha_csvd_field_logical_len(d->tgt, fb);
    uint32_t limit = max_bytes ? max_bytes : IBHA_CSVD_DEFAULT_MAX_CELL_BYTES;
    if (limit > SEG_MAX_CAP) limit = SEG_MAX_CAP;

    seg_out o;
    o.out = out;
    o.cap = out ? cap : 0u;
    o.n = 0;
    o.have_last = 0;
    o.last.op = 0;
    o.last.start = 0;
    o.last.len = 0;

    /* Over the cap, or one side empty: a whole value replacement, which carries
     * the same information a mode of NONE does. */
    if (la > limit || lb > limit || la == 0 || lb == 0) {
        seg_push(&o, IBHA_CSVD_SEG_DELETE, 0, la);
        seg_push(&o, IBHA_CSVD_SEG_INSERT, 0, lb);
        return (int)o.n;
    }

    seg_mem *m = seg_scratch(d, la > lb ? la : lb);
    if (!m) return IBHA_CSVD_ERR_OOM;

    /* ibha_csvd_field_copy is the one sanctioned place a value becomes a string,
     * and Myers needs random access, so this is where the boundary is crossed.
     * Both sides land in the scratch block and neither escapes it. */
    (void)ibha_csvd_field_copy(d->src, fa, m->a, m->cap);
    (void)ibha_csvd_field_copy(d->tgt, fb, m->b, m->cap);

    /*
     * Stripping the common prefix and suffix first is what keeps the cap from
     * biting on realistic edits: a 300 byte cell with one word changed becomes a
     * Myers over a handful of elements rather than over 300.
     */
    uint32_t pre = 0;
    while (pre < la && pre < lb && m->a[pre] == m->b[pre]) pre++;
    /*
     * Never cut inside a UTF-8 sequence, and never inside a word when the word
     * level is what is about to run: both would split an element and produce a
     * highlight that starts mid character or mid token. The byte after the cut
     * lives in whichever value still has one, since the two are equal up to here
     * and cannot both have run out.
     */
    while (pre > 0) {
        uint8_t next = pre < la ? m->a[pre] : m->b[pre];
        int split = (next & 0xC0u) == 0x80u;
        if (!split && mode != IBHA_CSVD_CELLDIFF_CHARACTER) {
            split = is_word_byte(next) && is_word_byte(m->a[pre - 1]);
        }
        if (!split) break;
        pre--;
    }

    uint32_t suf = 0;
    while (suf < la - pre && suf < lb - pre && m->a[la - 1 - suf] == m->b[lb - 1 - suf]) suf++;
    while (suf > 0) {
        /* Shrinking the suffix moves the cut to the right, off the partial
         * character or the half word and onto the next boundary. */
        uint8_t at = m->a[la - suf];
        int split = (at & 0xC0u) == 0x80u;
        if (!split && mode != IBHA_CSVD_CELLDIFF_CHARACTER && suf < la - pre) {
            split = is_word_byte(at) && is_word_byte(m->a[la - suf - 1]);
        }
        if (!split) break;
        suf--;
    }

    uint32_t amid = la - pre - suf;
    uint32_t bmid = lb - pre - suf;

    int word = (mode != IBHA_CSVD_CELLDIFF_CHARACTER);
    uint32_t na = tokenize(m->a + pre, amid, word, m->ea);
    uint32_t nb = tokenize(m->b + pre, bmid, word, m->eb);
    int n = myers(m, m->a + pre, m->ea, (int32_t)na, m->b + pre, m->eb, (int32_t)nb, m->ops);

    seg_push(&o, IBHA_CSVD_SEG_EQUAL, 0, pre);
    if (n < 0) {
        seg_push(&o, IBHA_CSVD_SEG_DELETE, pre, amid);
        seg_push(&o, IBHA_CSVD_SEG_INSERT, pre, bmid);
    } else {
        /* The element tables index from pre, so shift them once rather than
         * offsetting at every push. */
        for (uint32_t i = 0; i < na; i++) m->ea[2 * i] += pre;
        for (uint32_t i = 0; i < nb; i++) m->eb[2 * i] += pre;
        walk(m, &o, m->a, m->b, na, nb, pre, m->ops, n,
             mode == IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER);
    }
    seg_push(&o, IBHA_CSVD_SEG_EQUAL, la - suf, suf);
    return (int)o.n;
}
