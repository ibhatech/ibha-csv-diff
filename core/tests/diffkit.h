/*
 * diffkit.h - shared scaffolding for the Phase 2 suites.
 *
 * Every diff test needs the same three steps: parse a source with the four
 * header row model, parse a target against it with header auto-detection, and
 * run the diff. Doing that inline in each assertion would bury what is actually
 * being tested, so it lives here.
 *
 * Inputs are string literals and are borrowed rather than copied, which is legal
 * because a literal outlives the context.
 */
#ifndef IBHA_TEST_DIFFKIT_H
#define IBHA_TEST_DIFFKIT_H

#include <string.h>

#include "../src/internal.h"

typedef struct {
    ibha_csvd_ctx *ctx;
    ibha_csvd_parser *src;
    ibha_csvd_parser *tgt;
    ibha_csvd_diff *diff;
} kit_pair;

/* header_rows 4 is the full model with KEY, REQUIRED and type rows; 1 is a
 * names-only file, which has no key columns and so takes the all-keys path of
 * spec 6.4. */
static inline void kit_opts(ibha_csvd_parse_opts *o, uint32_t header_rows) {
    ibha_csvd_parse_opts_init(o);
    o->header.rows = header_rows;
    o->header.key_row = header_rows >= 4 ? 1 : 0;
    o->header.required_row = header_rows >= 4 ? 2 : 0;
    o->header.type_row = header_rows >= 4 ? 3 : 0;
    o->header.name_row = header_rows >= 1 ? header_rows : 0;
}

static inline int kit_parse(kit_pair *p, const char *src, const char *tgt, uint32_t header_rows,
                            const ibha_csvd_compare_opts *cmp) {
    memset(p, 0, sizeof(*p));
    p->ctx = ibha_csvd_ctx_new(NULL);
    if (!p->ctx) return 0;

    ibha_csvd_parse_opts so;
    kit_opts(&so, header_rows);
    if (cmp) so.compare = *cmp;
    p->src = ibha_csvd_parse_begin(p->ctx, &so);
    if (!p->src) return 0;
    if (ibha_csvd_parse_borrow(p->src, src, strlen(src)) != IBHA_CSVD_OK) return 0;
    if (ibha_csvd_parse_finish(p->src) != IBHA_CSVD_OK) return 0;

    ibha_csvd_parse_opts to;
    kit_opts(&to, header_rows);
    if (cmp) to.compare = *cmp;
    to.header.rows = IBHA_CSVD_HEADER_AUTO;
    to.expect_table = ibha_csvd_table_of(p->src);
    to.expect_schema = ibha_csvd_schema_of(p->src);
    p->tgt = ibha_csvd_parse_begin(p->ctx, &to);
    if (!p->tgt) return 0;
    if (ibha_csvd_parse_borrow(p->tgt, tgt, strlen(tgt)) != IBHA_CSVD_OK) return 0;
    if (ibha_csvd_parse_finish(p->tgt) != IBHA_CSVD_OK) return 0;
    return 1;
}

static inline int kit_diff(kit_pair *p, const ibha_csvd_diff_opts *opts) {
    ibha_csvd_diff_opts o;
    if (opts) {
        o = *opts;
    } else {
        ibha_csvd_diff_opts_init(&o);
    }
    p->diff = ibha_csvd_diff_run(p->ctx, ibha_csvd_table_of(p->src), ibha_csvd_schema_of(p->src),
                                 ibha_csvd_table_of(p->tgt), ibha_csvd_schema_of(p->tgt), &o);
    return p->diff != NULL;
}

static inline int kit_run(kit_pair *p, const char *src, const char *tgt, uint32_t header_rows) {
    return kit_parse(p, src, tgt, header_rows, NULL) && kit_diff(p, NULL);
}

static inline void kit_close(kit_pair *p) {
    ibha_csvd_ctx_free(p->ctx);
    memset(p, 0, sizeof(*p));
}

/*
 * The whole report as one letter per row: U unchanged, M modified, A added,
 * D deleted, with a trailing '>' on a row the matcher called moved. Report
 * ordering assertions read as a single string comparison this way, which is what
 * makes the anchored placement of spec 6.5 testable at a glance.
 */
static inline void kit_report(kit_pair *p, char *dst, size_t cap) {
    static const char k_letter[4] = {'U', 'M', 'A', 'D'};
    ibha_csvd_cursor *cur = ibha_csvd_cursor_open(p->diff);
    size_t w = 0;

    dst[0] = '\0';
    if (!cur) return;
    while (ibha_csvd_cursor_next(cur) == 1 && w + 3 < cap) {
        const ibha_csvd_row *r = ibha_csvd_cursor_row(cur);
        dst[w++] = k_letter[r->kind & 3u];
        if (r->moved) dst[w++] = '>';
    }
    dst[w] = '\0';
}

#endif /* IBHA_TEST_DIFFKIT_H */
