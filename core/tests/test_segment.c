/*
 * test_segment.c - the intra cell diff of spec 7.
 *
 * One property carries most of the weight here, and it is the one that makes a
 * diff renderable at all: **the segments must reconstruct both values**. Taking
 * the EQUAL and DELETE segments in order has to rebuild the source exactly, and
 * taking the EQUAL and INSERT segments has to rebuild the target exactly. A
 * highlight that fails this shows the reviewer text neither file contains, which
 * is worse than no highlight at all.
 *
 * Everything else is bounds: never split a UTF-8 sequence, never exceed the cap,
 * and never depend on the caller's buffer size for the answer.
 */
#include <string.h>

#include "diffkit.h"
#include "suites.h"
#include "tap.h"

#define HS "KEY,\nREQUIRED,\nVARCHAR(200),VARCHAR(200)\nid,text\n"
#define MAX_SEGS 64

typedef struct {
    kit_pair p;
    ibha_csvd_cursor *cur;
    const ibha_csvd_row *row;
    ibha_csvd_segment segs[MAX_SEGS];
    int n;
    uint8_t src[256];
    uint8_t tgt[256];
    uint32_t slen, tlen;
} seg_case;

/* Diffs one cell pair and collects everything an assertion might want. */
static int seg_run(seg_case *c, const char *a, const char *b, ibha_csvd_cell_diff_mode mode,
                   uint32_t max_bytes) {
    /*
     * Static, and that is not a style choice. kit_parse borrows rather than
     * copies, so the index points into these buffers and they have to outlive
     * this function; on the stack they would be dead the moment it returned and
     * the values would be whatever the next frame wrote there.
     */
    static char source[512], target[512];
    memset(c, 0, sizeof(*c));

    /* Quoted, because several of the fixtures carry a comma and a value with a
     * delimiter in it is a quoted field like any other. */
    snprintf(source, sizeof(source), HS "A,\"%s\"\n", a);
    snprintf(target, sizeof(target), HS "A,\"%s\"\n", b);
    if (!kit_run(&c->p, source, target, 4)) return 0;

    c->cur = ibha_csvd_cursor_open(c->p.diff);
    if (!c->cur || ibha_csvd_cursor_next(c->cur) != 1) return 0;
    c->row = ibha_csvd_cursor_row(c->cur);

    c->n = ibha_csvd_cell_segments(c->p.diff, c->row, 1, mode, max_bytes, c->segs, MAX_SEGS);

    uint32_t fs = ibha_csvd_row_field(ibha_csvd_table_of(c->p.src), c->row->source_row, 1);
    uint32_t ft = ibha_csvd_row_field(ibha_csvd_table_of(c->p.tgt), c->row->target_row, 1);
    c->slen = ibha_csvd_field_copy(ibha_csvd_table_of(c->p.src), fs, c->src, sizeof(c->src));
    c->tlen = ibha_csvd_field_copy(ibha_csvd_table_of(c->p.tgt), ft, c->tgt, sizeof(c->tgt));
    return 1;
}

/* Rebuilds one side from the segments. side 0 is the source, which takes EQUAL
 * and DELETE; side 1 is the target, which takes EQUAL and INSERT. */
static uint32_t seg_rebuild(const seg_case *c, int target_side, uint8_t *out, size_t cap) {
    uint32_t soff = 0, toff = 0, w = 0;
    for (int i = 0; i < c->n; i++) {
        uint32_t op = c->segs[i].op;
        uint32_t len = c->segs[i].len;
        const uint8_t *from = NULL;
        uint32_t at = 0;

        if (op == IBHA_CSVD_SEG_EQUAL) {
            from = target_side ? c->tgt : c->src;
            at = target_side ? toff : soff;
            soff += len;
            toff += len;
        } else if (op == IBHA_CSVD_SEG_DELETE) {
            if (!target_side) {
                from = c->src;
                at = soff;
            }
            soff += len;
        } else {
            if (target_side) {
                from = c->tgt;
                at = toff;
            }
            toff += len;
        }
        if (!from) continue;
        if (w + len > cap) return 0xFFFFFFFFu;
        memcpy(out + w, from + at, len);
        w += len;
    }
    return w;
}

static int rebuilds_both(seg_case *c) {
    uint8_t got[512];
    uint32_t n = seg_rebuild(c, 0, got, sizeof(got));
    if (n != c->slen || memcmp(got, c->src, n) != 0) return 0;
    n = seg_rebuild(c, 1, got, sizeof(got));
    if (n != c->tlen || memcmp(got, c->tgt, n) != 0) return 0;
    return 1;
}

/* ------------------------------------------------------- the spec 7 case -- */

static void test_the_example(void) {
    seg_case c;

    /* Spec 7's own example. Word-then-character should leave "Accident " alone,
     * mark the capital V and add "(s)", rather than replacing the whole cell. */
    if (seg_run(&c, "Accident violation code", "Accident Violation code(s)",
                IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER, 0)) {
        TAP_OK(c.n > 0, "the spec 7 example produces segments");
        TAP_OK(rebuilds_both(&c), "and they rebuild both values exactly");

        uint32_t equal_bytes = 0, changed_bytes = 0;
        for (int i = 0; i < c.n; i++) {
            if (c.segs[i].op == IBHA_CSVD_SEG_EQUAL) {
                equal_bytes += c.segs[i].len;
            } else {
                changed_bytes += c.segs[i].len;
            }
        }
        /* The whole point: most of the cell is untouched. A whole cell
         * replacement would report 23 deleted and 26 inserted. */
        TAP_OK(equal_bytes >= 20, "most of the cell is reported unchanged");
        TAP_OK(changed_bytes <= 8, "and only the edit is highlighted");
    } else {
        TAP_OK(0, "the spec 7 example produces segments");
        TAP_OK(0, "and they rebuild both values exactly");
        TAP_OK(0, "most of the cell is reported unchanged");
        TAP_OK(0, "and only the edit is highlighted");
    }
    kit_close(&c.p);

    /* Word mode alone must not split a token: replacing "violation" reports the
     * whole token, not the single letter inside it. */
    if (seg_run(&c, "Accident violation code", "Accident Violation code",
                IBHA_CSVD_CELLDIFF_WORD, 0)) {
        int whole_token = 0;
        for (int i = 0; i < c.n; i++) {
            if (c.segs[i].op == IBHA_CSVD_SEG_DELETE && c.segs[i].len == 9) whole_token = 1;
        }
        TAP_OK(whole_token, "word mode replaces the whole token");
        TAP_OK(rebuilds_both(&c), "and rebuilds both values");
    } else {
        TAP_OK(0, "word mode replaces the whole token");
        TAP_OK(0, "and rebuilds both values");
    }
    kit_close(&c.p);

    if (seg_run(&c, "Accident violation code", "Accident Violation code",
                IBHA_CSVD_CELLDIFF_CHARACTER, 0)) {
        int one_byte = 0;
        for (int i = 0; i < c.n; i++) {
            if (c.segs[i].op != IBHA_CSVD_SEG_EQUAL && c.segs[i].len == 1) one_byte = 1;
        }
        TAP_OK(one_byte, "character mode narrows the same edit to one byte");
        TAP_OK(rebuilds_both(&c), "and rebuilds both values");
    } else {
        TAP_OK(0, "character mode narrows the same edit to one byte");
        TAP_OK(0, "and rebuilds both values");
    }
    kit_close(&c.p);
}

/* ------------------------------------------------------- the rebuild law -- */

static void test_rebuilds(void) {
    static const char *const k_pairs[][2] = {
        {"abc", "abd"},
        {"abc", "xyz"},
        {"", "inserted"},
        {"deleted", ""},
        {"one two three", "one three"},
        {"one three", "one two three"},
        {"a,b,c", "a,b,c,d"},
        {"NORTH", "north"},
        {"2026-01-15", "2026-02-15"},
        {"the quick brown fox", "the quick red fox jumped"},
        {"caf\xC3\xA9 ltd", "caf\xC3\xA9 limited"},
        {"\xE6\x97\xA5\xE6\x9C\xAC", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"},
    };
    static const ibha_csvd_cell_diff_mode k_modes[3] = {IBHA_CSVD_CELLDIFF_WORD,
                                                        IBHA_CSVD_CELLDIFF_CHARACTER,
                                                        IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER};
    seg_case c;
    int all_rebuild = 1, all_bounded = 1, all_utf8 = 1;

    for (size_t i = 0; i < sizeof(k_pairs) / sizeof(k_pairs[0]); i++) {
        for (size_t m = 0; m < 3; m++) {
            if (!seg_run(&c, k_pairs[i][0], k_pairs[i][1], k_modes[m], 0)) {
                all_rebuild = 0;
                kit_close(&c.p);
                continue;
            }
            if (c.n > 0 && c.n <= MAX_SEGS && !rebuilds_both(&c)) all_rebuild = 0;

            for (int s = 0; s < c.n && s < MAX_SEGS; s++) {
                const uint8_t *v = c.segs[s].op == IBHA_CSVD_SEG_INSERT ? c.tgt : c.src;
                uint32_t vlen = c.segs[s].op == IBHA_CSVD_SEG_INSERT ? c.tlen : c.slen;
                if (c.segs[s].start + c.segs[s].len > vlen) all_bounded = 0;
                /* Never start or end in the middle of a UTF-8 sequence. */
                if (c.segs[s].len && (v[c.segs[s].start] & 0xC0u) == 0x80u) all_utf8 = 0;
            }
            kit_close(&c.p);
        }
    }
    TAP_OK(all_rebuild, "every mode on every pair rebuilds both values exactly");
    TAP_OK(all_bounded, "and every segment lies inside the value it indexes");
    TAP_OK(all_utf8, "and no segment begins inside a UTF-8 sequence");
}

/* ------------------------------------------------------------- the edges -- */

static void test_edges(void) {
    seg_case c;

    if (seg_run(&c, "same", "different", IBHA_CSVD_CELLDIFF_NONE, 0)) {
        TAP_EQ_U64((uint64_t)c.n, 0, "mode NONE computes nothing");
    } else {
        TAP_OK(0, "mode NONE computes nothing");
    }
    kit_close(&c.p);

    /* An unchanged cell of a modified row has nothing to segment. */
    if (seg_run(&c, "x", "y", IBHA_CSVD_CELLDIFF_WORD, 0)) {
        int n = ibha_csvd_cell_segments(c.p.diff, c.row, 0, IBHA_CSVD_CELLDIFF_WORD, 0, c.segs,
                                        MAX_SEGS);
        TAP_EQ_U64((uint64_t)n, 0, "an unchanged cell produces no segments");
        n = ibha_csvd_cell_segments(c.p.diff, c.row, 99, IBHA_CSVD_CELLDIFF_WORD, 0, c.segs,
                                    MAX_SEGS);
        TAP_OK(n == IBHA_CSVD_ERR_INVALID_ARG, "a column past the end is refused");
    } else {
        TAP_OK(0, "an unchanged cell produces no segments");
        TAP_OK(0, "a column past the end is refused");
    }
    kit_close(&c.p);

    /* An added row has no source value, so there is nothing to compare against
     * and the answer is no segments rather than a crash. */
    kit_pair p;
    if (kit_run(&p, HS "A,one\n", HS "A,one\nB,two\n", 4)) {
        ibha_csvd_cursor *cur = ibha_csvd_cursor_open(p.diff);
        ibha_csvd_segment segs[4];
        int n = -1;
        while (cur && ibha_csvd_cursor_next(cur) == 1) {
            const ibha_csvd_row *r = ibha_csvd_cursor_row(cur);
            if (r->kind != IBHA_CSVD_ROW_ADDED) continue;
            n = ibha_csvd_cell_segments(p.diff, r, 1, IBHA_CSVD_CELLDIFF_WORD, 0, segs, 4);
        }
        TAP_EQ_U64((uint64_t)n, 0, "an added row has nothing to segment");
    } else {
        TAP_OK(0, "an added row has nothing to segment");
    }
    kit_close(&p);

    /* Past the cap the answer is a whole value replacement, which carries the
     * same information a mode of NONE does and costs no Myers run. */
    if (seg_run(&c, "abcdefghij", "abcdefghXX", IBHA_CSVD_CELLDIFF_CHARACTER, 4)) {
        TAP_EQ_U64((uint64_t)c.n, 2, "a pair over max_bytes reports one delete and one insert");
        TAP_OK(c.segs[0].op == IBHA_CSVD_SEG_DELETE && c.segs[0].len == 10 &&
                   c.segs[1].op == IBHA_CSVD_SEG_INSERT && c.segs[1].len == 10,
               "covering both values whole");
    } else {
        TAP_OK(0, "a pair over max_bytes reports one delete and one insert");
        TAP_OK(0, "covering both values whole");
    }
    kit_close(&c.p);

    /*
     * The count must not depend on the caller's buffer. A caller that sizes a
     * buffer from a first call and calls again would loop forever if a small cap
     * inflated the answer.
     */
    if (seg_run(&c, "a b c d e f g", "a X c Y e Z g", IBHA_CSVD_CELLDIFF_WORD, 0)) {
        ibha_csvd_segment one[1];
        int full = c.n;
        int small = ibha_csvd_cell_segments(c.p.diff, c.row, 1, IBHA_CSVD_CELLDIFF_WORD, 0, one, 1);
        int none = ibha_csvd_cell_segments(c.p.diff, c.row, 1, IBHA_CSVD_CELLDIFF_WORD, 0, NULL, 0);
        TAP_OK(full > 1, "the multi edit fixture needs several segments");
        TAP_EQ_U64((uint64_t)small, (uint64_t)full, "a short buffer returns the same count");
        TAP_EQ_U64((uint64_t)none, (uint64_t)full, "and so does a NULL buffer");
        TAP_OK(one[0].op == c.segs[0].op && one[0].start == c.segs[0].start &&
                   one[0].len == c.segs[0].len,
               "and the segments it did write are the first ones");
    } else {
        TAP_OK(0, "the multi edit fixture needs several segments");
        TAP_OK(0, "a short buffer returns the same count");
        TAP_OK(0, "and so does a NULL buffer");
        TAP_OK(0, "and the segments it did write are the first ones");
    }
    kit_close(&c.p);

    /* The scratch is allocated once and reused, so the hundredth cell diff costs
     * no more memory than the first. */
    if (seg_run(&c, "alpha beta gamma", "alpha delta gamma", IBHA_CSVD_CELLDIFF_WORD, 0)) {
        uint64_t after_first = ibha_csvd_ctx_bytes_reserved(c.p.ctx);
        for (int i = 0; i < 100; i++) {
            (void)ibha_csvd_cell_segments(c.p.diff, c.row, 1, IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER,
                                          0, c.segs, MAX_SEGS);
        }
        TAP_EQ_U64(ibha_csvd_ctx_bytes_reserved(c.p.ctx), after_first,
                   "a hundred more cell diffs reuse the same scratch");
    } else {
        TAP_OK(0, "a hundred more cell diffs reuse the same scratch");
    }
    kit_close(&c.p);
}

void ibha_test_segment(void) {
    test_the_example();
    test_rebuilds();
    test_edges();
}
