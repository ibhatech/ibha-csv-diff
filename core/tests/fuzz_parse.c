/*
 * fuzz_parse.c - libFuzzer entry point for the RFC 4180 state machine.
 *
 * The parser is the only place in the engine where untrusted bytes drive control
 * flow, and spec 2.3 named fuzzing it as the main reason C was judged acceptable
 * here. So this target does more than check for crashes: it asserts the
 * invariants that a memory safe language would give for free, and the one
 * property that no language gives for free, which is that chunking is invisible.
 *
 *   make fuzz
 *   build/fuzz_parse -max_total_time=60 corpus/
 */
#include <stddef.h>
#include <stdint.h>

#include "../src/internal.h"

typedef struct {
    const uint8_t *bytes;
    size_t len, pos, stride;
} fuzz_reader;

static int64_t fuzz_read(void *read_ctx, uint8_t *dst, size_t cap) {
    fuzz_reader *r = (fuzz_reader *)read_ctx;
    size_t left = r->len - r->pos;
    if (left == 0) return 0;
    size_t n = left < r->stride ? left : r->stride;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) dst[i] = r->bytes[r->pos + i];
    r->pos += n;
    return (int64_t)n;
}

/* Every index entry must address bytes that exist, rows must be monotonic, and
 * the sentinel must close the last row. A violation here is a wrong answer that
 * ASan would not necessarily catch, because the memory is all inside the arena. */
static void check_index(const ibha_csvd_table *t) {
    if (t->n_rows == 0 && t->n_fields != 0) __builtin_trap();

    for (uint32_t f = 0; f < t->n_fields; f++) {
        if ((uint64_t)t->field_off[f] + t->field_len[f] > (uint64_t)t->len) __builtin_trap();
        if ((t->field_flags[f] & IBHA_CSVD_FIELD_EMPTY) && t->field_len[f] != 0) __builtin_trap();
    }
    for (uint32_t r = 0; r < t->n_rows; r++) {
        if (t->row_first_field[r] > t->row_first_field[r + 1]) __builtin_trap();
        /* Ragged rows are either normalized or rejected, so every surviving row
         * has exactly the column count. */
        if (t->row_first_field[r + 1] - t->row_first_field[r] != t->n_columns) __builtin_trap();
    }
    if (t->n_rows && t->row_first_field[t->n_rows] != t->n_fields) __builtin_trap();
}

static int tables_same(const ibha_csvd_table *a, const ibha_csvd_table *b) {
    if (a->n_rows != b->n_rows || a->n_fields != b->n_fields || a->n_columns != b->n_columns) {
        return 0;
    }
    for (uint32_t f = 0; f < a->n_fields; f++) {
        if (a->field_off[f] != b->field_off[f] || a->field_len[f] != b->field_len[f] ||
            a->field_flags[f] != b->field_flags[f]) {
            return 0;
        }
    }
    for (uint32_t r = 0; r < a->n_rows; r++) {
        if (a->row_key_hash[r] != b->row_key_hash[r]) return 0;
        if (a->row_full_hash[r] != b->row_full_hash[r]) return 0;
    }
    return 1;
}

static void opts_init(ibha_csvd_parse_opts *o, uint8_t control, uint64_t hint) {
    ibha_csvd_parse_opts_init(o);
    /* Exercise the headerless, names-only and four row models. Auto-detection
     * needs a source schema and is covered by the unit tests instead. */
    o->header.rows = (uint32_t)(control % 3u) * 2u; /* 0, 2 or 4 */
    o->header.key_row = o->header.rows >= 1 ? 1 : 0;
    o->header.required_row = o->header.rows >= 2 ? 2 : 0;
    o->header.type_row = o->header.rows >= 4 ? 3 : 0;
    o->header.name_row = o->header.rows;
    o->dialect.strip_bom = (control & 0x40u) ? 1 : 0;
    o->hash_rows = (control & 0x80u) ? 0 : 1;
    o->size_hint = hint;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3) return 0;

    /* The first two bytes steer the harness rather than the parser: a chunk
     * stride and an options selector, so one corpus entry explores many splits
     * and header models. */
    size_t stride = (size_t)data[0] + 1;
    uint8_t control = data[1];
    const uint8_t *payload = data + 2;
    size_t payload_len = size - 2;

    ibha_csvd_limits lim;
    ibha_csvd_limits_init(&lim);
    lim.max_bytes = 1024 * 1024;

    /* Pass one: streamed at the chosen stride. */
    ibha_csvd_ctx *c1 = ibha_csvd_ctx_new(&lim);
    if (!c1) return 0;
    ibha_csvd_parse_opts o1;
    opts_init(&o1, control, (control & 0x20u) ? (uint64_t)payload_len : 0);

    fuzz_reader r = {payload, payload_len, 0, stride};
    ibha_csvd_parser *p1 = NULL;
    ibha_csvd_parse_stream(c1, fuzz_read, &r, &o1, &p1);
    ibha_csvd_status s1 = ibha_csvd_ctx_status(c1);
    if (p1 && s1 == IBHA_CSVD_OK) check_index(ibha_csvd_table_of(p1));

    /* Pass two: the whole input at once, borrowed. The two must agree on both
     * the outcome and the index, which is the property that makes streaming
     * safe. Any divergence is a resumability bug. */
    ibha_csvd_ctx *c2 = ibha_csvd_ctx_new(&lim);
    if (c2) {
        ibha_csvd_parse_opts o2;
        opts_init(&o2, control, 0);
        ibha_csvd_parser *p2 = ibha_csvd_parse_begin(c2, &o2);
        if (p2) {
            ibha_csvd_parse_borrow(p2, payload, payload_len);
            ibha_csvd_parse_finish(p2);
            ibha_csvd_status s2 = ibha_csvd_ctx_status(c2);

            if (s1 != s2) __builtin_trap();
            if (s1 == IBHA_CSVD_OK && p1) {
                if (!tables_same(ibha_csvd_table_of(p1), ibha_csvd_table_of(p2))) __builtin_trap();

                /* Reading a cell back must never overrun, and the logical length
                 * must agree with what a copy produces. */
                const ibha_csvd_table *t = ibha_csvd_table_of(p2);
                for (uint32_t f = 0; f < t->n_fields && f < 4096; f++) {
                    uint8_t buf[256];
                    uint32_t need = ibha_csvd_field_logical_len(t, f);
                    if (need > t->field_len[f]) __builtin_trap();
                    if (need <= sizeof(buf)) {
                        if (ibha_csvd_field_copy(t, f, buf, sizeof(buf)) != need) __builtin_trap();
                    }
                    (void)ibha_csvd_field_hash(t, f);
                }
            }
        }
        ibha_csvd_ctx_free(c2);
    }

    ibha_csvd_ctx_free(c1);
    return 0;
}
