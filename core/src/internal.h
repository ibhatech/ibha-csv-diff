/*
 * internal.h - shared internals. Not installed, not part of the ABI.
 */
#ifndef IBHA_CSVD_INTERNAL_H
#define IBHA_CSVD_INTERNAL_H

#include "ibha_csvdiff.h"

/* ------------------------------------------------------------ system layer -- */
/*
 * The only two calls the engine makes to the outside world for memory. Two
 * implementations: sys_libc.c for native builds, sys_wasm.c for the freestanding
 * wasm32 build where there is no libc and memory comes from memory.grow.
 */
void *ibha_csvd_sys_alloc(size_t n);
void ibha_csvd_sys_free(void *p, size_t n);

/* Freestanding builds have no libc, so we provide the handful of routines the
 * compiler emits calls to and that the engine uses directly. */
#ifdef __wasm__
void *ibha_csvd_memcpy(void *dst, const void *src, size_t n);
void *ibha_csvd_memset(void *dst, int c, size_t n);
int ibha_csvd_memcmp(const void *a, const void *b, size_t n);
size_t ibha_csvd_strlen(const char *s);
#define IBHA_MEMCPY ibha_csvd_memcpy
#define IBHA_MEMSET ibha_csvd_memset
#define IBHA_MEMCMP ibha_csvd_memcmp
#define IBHA_STRLEN ibha_csvd_strlen
#else
#include <string.h>
#define IBHA_MEMCPY memcpy
#define IBHA_MEMSET memset
#define IBHA_MEMCMP memcmp
#define IBHA_STRLEN strlen
#endif

/* ------------------------------------------------------------------- arena -- */
/*
 * Chunked bump allocator. Individual allocations are never freed; the whole
 * arena dies with the context. This is what keeps two concurrent diffs from
 * touching any shared allocator state, per spec 2.6.3.
 */
#define IBHA_ARENA_ALIGN 16u
#define IBHA_ARENA_FIRST_BLOCK (64u * 1024u)
#define IBHA_ARENA_MAX_BLOCK (32u * 1024u * 1024u)

typedef struct ibha_arena_block {
    struct ibha_arena_block *next;
    size_t cap;
    size_t used;
    uint8_t *data;
} ibha_arena_block;

typedef struct {
    ibha_arena_block *head;
    uint64_t reserved;
    uint64_t used;
} ibha_arena;

void ibha_arena_init(ibha_arena *a);
void ibha_arena_destroy(ibha_arena *a);

/* Returns NULL on exhaustion. Result is IBHA_ARENA_ALIGN aligned and zeroed
 * only when ibha_arena_calloc is used. */
void *ibha_arena_alloc(ibha_arena *a, size_t n);
void *ibha_arena_calloc(ibha_arena *a, size_t n);

/*
 * For allocations large enough that rounding them up to a block size wastes real
 * memory: takes a dedicated block sized exactly to the request and leaves the
 * current block as the bump target, so the remaining space in it is still used.
 * Serving a 10 MB index array out of the ordinary path would otherwise reserve a
 * 32 MB block and abandon whatever was left of the previous one.
 */
void *ibha_arena_alloc_large(ibha_arena *a, size_t n);

/*
 * Growable byte buffer backed by the arena. Used by ingest to accumulate the
 * source bytes contiguously, which every later phase needs because the field
 * index stores offsets into one flat region.
 *
 * Growth reallocates out of the arena and copies. The arena does not reclaim the
 * old block, so the buffer is seeded from a size hint where one is available and
 * doubles otherwise. Wasted bytes are bounded by the final size.
 */
typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t cap;
} ibha_bytebuf;

int ibha_bytebuf_reserve(ibha_arena *a, ibha_bytebuf *b, size_t need);
int ibha_bytebuf_append(ibha_arena *a, ibha_bytebuf *b, const uint8_t *src, size_t n);

/* ----------------------------------------------------------------- context -- */

#define IBHA_ERRMSG_CAP 256

struct ibha_csvd_ctx {
    ibha_arena arena;
    ibha_csvd_limits limits;
    ibha_csvd_status status;
    char errmsg[IBHA_ERRMSG_CAP];
    ibha_bytebuf ingested;
    int has_ingested;
};

/*
 * Records an error. First one wins, per spec 13.5: subsequent calls are ignored
 * so the original cause is never overwritten by a downstream symptom. Always
 * returns the status it was given, so call sites can `return ibha_err(...)`.
 */
ibha_csvd_status ibha_err(ibha_csvd_ctx *ctx, ibha_csvd_status st, const char *fmt, ...);

/* ------------------------------------------------------------------ hash -- */
/*
 * XXH3, 64 bit, one shot, unseeded, default secret. That subset is what the
 * engine needs and it is verified bit for bit against the reference
 * implementation in tests/test_hash.c, so a future digest endpoint computed with
 * stock xxHash agrees with us.
 */
uint64_t ibha_xxh3_64(const void *data, size_t len);

/* Order dependent combiner for folding field hashes into a row hash. */
uint64_t ibha_hash_mix(uint64_t acc, uint64_t h);
uint64_t ibha_hash_final(uint64_t acc, uint64_t n);

/*
 * The 64x64 to 128 multiply XXH3 is built on, and the hand rolled version of it.
 * Both are declared because the test asserts they agree: the wasm32 build uses
 * the portable one and the native build does not, and spec 3.2 requires the two
 * targets to produce byte identical output.
 */
uint64_t ibha_mul128_fold64(uint64_t a, uint64_t b);
uint64_t ibha_mul128_fold64_portable(uint64_t a, uint64_t b);

/*
 * Longest logical run hashed in one XXH3 call. A field longer than this is
 * folded chunk by chunk over its *logical* bytes, which is what lets the escaped
 * path collapse "" through a fixed stack window and still produce the same value
 * as the unescaped path. Nothing here allocates.
 */
#define IBHA_HASH_CHUNK 4096u

/* ----------------------------------------------------------------- field -- */
/*
 * The logical value comparator of spec 5.2. Walks both fields at once,
 * collapsing "" to " on whichever side carries escapes, and never allocates.
 * Quoting is invisible to it by construction.
 */
int ibha_field_cmp_raw(const uint8_t *a, uint32_t alen, int a_esc, const uint8_t *b, uint32_t blen,
                       int b_esc, uint8_t quote);

uint64_t ibha_field_hash_raw(const uint8_t *p, uint32_t len, int esc, uint8_t quote);
uint32_t ibha_field_logical_len_raw(const uint8_t *p, uint32_t len, int esc, uint8_t quote);
uint32_t ibha_field_copy_raw(const uint8_t *p, uint32_t len, int esc, uint8_t quote, uint8_t *dst,
                             size_t cap);

/* ---------------------------------------------------------------- schema -- */

/*
 * Resolves the header model once enough rows exist. header_rows is the number of
 * header rows the file turned out to have; passing IBHA_CSVD_HEADER_AUTO means
 * auto-detection failed and asks for the diagnosis below, so that call always
 * returns an error. Defined in schema.c.
 */
ibha_csvd_status ibha_schema_resolve(ibha_csvd_ctx *ctx, const ibha_csvd_table *tbl,
                                     const ibha_csvd_header_opts *opt,
                                     const ibha_csvd_table *expect_tbl,
                                     const ibha_csvd_schema *expect_schema, uint32_t header_rows,
                                     const ibha_csvd_compare_opts *cmp, ibha_csvd_schema *out);

/* Parses the declared types out of the type row into col_type, col_size and
 * col_scale. When inherit is non NULL the target copies the source's types
 * rather than reading its own, per spec 13.8. map, when non NULL, says which
 * source column each of this file's columns corresponds to, which is how the
 * column policy of spec 6.6 inherits by name rather than by position. Defined in
 * normalize.c. */
ibha_csvd_status ibha_types_resolve(ibha_csvd_ctx *ctx, const ibha_csvd_table *tbl,
                                    const ibha_csvd_schema *inherit, const uint32_t *map,
                                    ibha_csvd_schema *out);

/* Explains why no row matched the source's column names: a reordered header, a
 * different column count, or genuinely no header row. Always returns an error. */
ibha_csvd_status ibha_schema_no_header(ibha_csvd_ctx *ctx, const ibha_csvd_table *tbl,
                                       const ibha_csvd_table *expect_tbl,
                                       const ibha_csvd_schema *expect_schema,
                                       const ibha_csvd_compare_opts *cmp);

/* True when row `row` of tbl is the column name row of expect_schema. Used by
 * the parser to stop the auto-detection scan at the first matching row. Under a
 * lenient column policy "is the header row" means "carries the source's columns
 * in order, possibly with additions or omissions". */
int ibha_schema_row_matches_names(const ibha_csvd_table *tbl, uint32_t row,
                                  const ibha_csvd_table *expect_tbl,
                                  const ibha_csvd_schema *expect_schema,
                                  const ibha_csvd_compare_opts *cmp);

/*
 * Fills map[c] with the source column that column c of tbl's header row
 * corresponds to, or IBHA_CSVD_NO_COLUMN, and returns how many matched. Returns
 * 0 when the row cannot be reconciled with the source's columns at all, which
 * includes any reordering: spec 13.10 has no flag for that.
 */
uint32_t ibha_schema_column_map(const ibha_csvd_table *tbl, uint32_t row,
                                const ibha_csvd_table *expect_tbl,
                                const ibha_csvd_schema *expect_schema,
                                const ibha_csvd_compare_opts *cmp, uint32_t *map);

/* ------------------------------------------------------------- normalize -- */
/*
 * The comparator selection of spec 5.3, expressed so that comparison and
 * hashing cannot drift apart.
 *
 * Every declared type reduces a cell to one *normalized byte sequence*, and
 * exactly one pair of primitives consumes it: ibha_norm_cmp and ibha_norm_hash.
 * That is what makes the row digests and the cell comparators agree by
 * construction rather than by two implementations being kept in step. If the
 * comparator says 1.50 equals 1.5, the digests say so too, and the unchanged-row
 * fast path of spec 6.1 step 3 stays correct.
 *
 * The normalized form is usually a *view* over the field's own bytes, trimmed,
 * with the "" pairs still in place and collapsed on the fly exactly as Phase 1
 * already does. Only the numeric and boolean canonical forms are built, and they
 * are built into a fixed stack buffer. Nothing here allocates.
 */

/* Enough for a canonical DECIMAL(38,s) with a sign and a point. A value whose
 * canonical form does not fit falls back to trimmed byte comparison, which is
 * the fallback spec 5.3 already names for out of range decimals. */
#define IBHA_NORM_SCRATCH 48u

/* How many column signatures an unmatched row is indexed under in the spec 6.4
 * similarity pairing. Spec 6.4 suggests "the hash of each individual column
 * value, or the hash of the first few non empty columns"; the second bounds the
 * index by row rather than by row times column count, which is what keeps a wide
 * table from turning the candidate index into the largest thing in the arena. */
#define IBHA_SIG_COLUMNS 8u

typedef struct {
    const uint8_t *p;
    uint32_t len;
    int esc; /* p still contains "" pairs to collapse while walking */
} ibha_norm;

/*
 * Fills out with the normalized view of field f under the declared type.
 * scratch must have room for IBHA_NORM_SCRATCH bytes and must outlive out,
 * because out may point into it.
 *
 * Returns 1 when normalization changed the value from its raw logical form,
 * which is what lets the raw digest reuse the normalized hash for the majority
 * of cells instead of hashing everything twice.
 */
int ibha_normalize(const ibha_csvd_table *t, uint32_t f, uint8_t type,
                   const ibha_csvd_compare_opts *o, uint8_t *scratch, ibha_norm *out);

int ibha_norm_cmp(const ibha_norm *a, const ibha_norm *b, uint8_t quote);
uint64_t ibha_norm_hash(const ibha_norm *a, uint8_t quote);

/*
 * Folds the three row digests for one row, over the columns the schema declares.
 * Two callers: the parser as each row completes, and the column projection of
 * spec 6.6 when the two files do not carry the same columns.
 */
void ibha_hash_row(const ibha_csvd_table *t, const ibha_csvd_schema *s,
                   const ibha_csvd_compare_opts *o, uint32_t row);

/*
 * The canonical decimal form, shared with the validator. Writes at most
 * IBHA_NORM_SCRATCH bytes and returns 0 when the value is not a number or when
 * its canonical form does not fit.
 *
 * The validator uses this rather than a scanner of its own, deliberately: "does
 * not parse as its declared DECIMAL" has to mean exactly what the comparator
 * means by a number, or a cell could be reported as unparseable in the findings
 * and compared as a number in the same report.
 */
int ibha_canonical_decimal(const uint8_t *p, uint32_t len, uint8_t *dst, uint32_t *out_len);

/*
 * The canonical TIMESTAMP form: insignificant trailing zeros removed from the
 * fractional seconds, so 14:22:05 equals 14:22:05.000. Writes at most
 * IBHA_NORM_SCRATCH bytes.
 *
 * Returns 0 both when the value has no fraction to shorten and when it is
 * already canonical, because in either case the trimmed bytes are the answer and
 * copying them would be wasted work.
 */
int ibha_canonical_timestamp(const uint8_t *p, uint32_t len, uint8_t *dst, uint32_t *out_len);

/* Fills in the NULL truth sets and clamps nonsense values, so every consumer
 * sees one canonical form of the options. */
void ibha_compare_opts_resolve(ibha_csvd_compare_opts *o);

/*
 * Digest of everything the row digests depend on: the comparison settings and
 * the parts of the schema that select a comparator. Two tables whose ids differ
 * cannot be diffed, because their digests mean different things.
 */
uint64_t ibha_compare_id(const ibha_csvd_compare_opts *o, const ibha_csvd_schema *s);

/* ------------------------------------------------------------------ diff -- */
/*
 * Row indices inside the diff are *data relative*: 0 is the first data row, so
 * absolute row = first_data_row + i. The public row struct converts back, which
 * is the only place the two conventions meet.
 */

struct ibha_csvd_diff {
    ibha_csvd_ctx *ctx;
    const ibha_csvd_table *src;
    const ibha_csvd_table *tgt;
    const ibha_csvd_schema *ss;
    const ibha_csvd_schema *ts;
    ibha_csvd_diff_opts opts;
    ibha_csvd_diff_stats stats;

    uint32_t s_first, t_first; /* first data row, absolute */
    uint32_t n_src, n_tgt;     /* data row counts */
    uint32_t n_columns;

    uint32_t *t2s; /* n_tgt entries, source data row or IBHA_CSVD_NO_ROW */
    uint32_t *s2t; /* n_src entries, target data row or IBHA_CSVD_NO_ROW */

    uint8_t *t_moved;      /* n_tgt entries, per spec 6.2 */
    int32_t *t_move_dist;  /* n_tgt entries */

    /* Anchored deletion buckets, per spec 6.5. del_head[0] holds the source rows
     * deleted before anything matched; del_head[t + 1] those anchored after
     * target data row t. Lists are in source order, so consecutive deletions
     * stay grouped. Placing every deletion in bucket n_tgt is exactly
     * IBHA_CSVD_DELETED_END, so one walk serves both placements. */
    uint32_t *del_head; /* n_tgt + 1 entries */
    uint32_t *del_next; /* n_src entries */

    /* Per column validation plan, spec 13.5. n_columns entries of IBHA_CHK_*, or
     * NULL when validation is off. checks_any is the pre-filter: a schema with no
     * REQUIRED column, no declared length and no numeric type has nothing to
     * check, and then the cursor never looks at a cell it would not otherwise
     * have read. */
    uint8_t *col_check;
    int checks_any;

    /* Scratch for ibha_csvd_cell_segments, allocated on first use and reused.
     * Bounded by the cell diff cap, so it never grows with the diff. */
    void *seg_scratch;
    size_t seg_scratch_cap;

    /*
     * The column projection of spec 6.6, in force only when the two files do not
     * carry the same columns and the policy allows it. src, tgt, ss and ts above
     * then point at projected copies holding exactly the compared columns in the
     * source's order, which is what lets every other line in the engine keep
     * treating column c as meaning the same thing on both sides.
     *
     * The originals are kept because the summary has to name the columns that
     * were added or removed, and those exist only in the unprojected tables.
     */
    int projected;
    const ibha_csvd_table *src0;
    const ibha_csvd_table *tgt0;
    const ibha_csvd_schema *ss0;
    const ibha_csvd_schema *ts0;
    uint32_t *added_cols;   /* physical columns of tgt0, stats.columns_added of them */
    uint32_t *removed_cols; /* physical columns of src0, stats.columns_removed of them */
};

/* ------------------------------------------------------------- validate -- */
/*
 * Spec 13.5's validation findings: output rather than errors. Which columns are
 * worth looking at is decided once, in ibha_validate_plan; what a cell is worth
 * saying is decided per cell, as the cursor reaches it.
 */
#define IBHA_CHK_REQUIRED 0x01u /* REQUIRED column: an empty value is a finding */
#define IBHA_CHK_LENGTH 0x02u   /* VARCHAR(n) or CHAR(n): count the characters */
#define IBHA_CHK_NUMERIC 0x04u  /* DECIMAL or INTEGER: parse, and check p and s */

ibha_csvd_status ibha_validate_plan(ibha_csvd_diff *d);

/*
 * ORs the findings for one absolute table row into flags, one entry per column,
 * and adds them to the diff's counters. Returns the number of cells that carry at
 * least one finding.
 */
uint32_t ibha_validate_row(ibha_csvd_diff *d, const ibha_csvd_table *tbl, uint32_t row,
                           uint8_t *flags);

/* Matches the two sides and fills t2s, s2t and the move flags. Defined in
 * match.c. */
ibha_csvd_status ibha_match(ibha_csvd_diff *d);

/* Normalized equality of one column between a source row and a target row, both
 * absolute table row indices. The comparator is the one the column's declared
 * type selects, so this agrees with the row digests by construction. */
int ibha_cell_equal(const ibha_csvd_diff *d, uint32_t srow, uint32_t trow, uint32_t col);

#endif /* IBHA_CSVD_INTERNAL_H */
