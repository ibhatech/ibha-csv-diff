/*
 * ibha_csvdiff.h - public ABI for the ibha-csvdiff engine.
 *
 * Design rules this header obeys, from specs/02-solution-design.md section 13:
 *   - No global mutable state. Every entry point takes an ibha_csvd_ctx *.
 *   - Ingest is a pull callback. Streaming is the only path; one shot is an adapter.
 *   - One error per context, first one wins. No error accumulation.
 *   - Nothing here allocates a string on the caller's behalf.
 *
 * Phases 0 to 3 implement the context and the arena, the parser and the columnar
 * index, the diff engine and its cursor, and the emitters. Symbols declared but
 * not yet implemented are marked PHASE N and return IBHA_CSVD_ERR_UNIMPLEMENTED.
 */
#ifndef IBHA_CSVDIFF_H
#define IBHA_CSVDIFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IBHA_CSVD_VERSION_MAJOR 0
#define IBHA_CSVD_VERSION_MINOR 1
#define IBHA_CSVD_VERSION_PATCH 0

/*
 * Marks a symbol as part of the installed API.
 *
 * On the freestanding wasm32 build it is what makes a function reachable from
 * JavaScript: that build compiles with -fvisibility=hidden and links with
 * --gc-sections, so a function nothing marks as exported is removed from the
 * module entirely. This is not a theoretical concern. Before this macro existed
 * the wasm module linked cleanly and was 292 bytes containing nothing but its
 * memory, because every symbol in it was hidden and then collected.
 *
 * The rule that keeps it honest: **this appears on exactly the declarations in
 * this header and nowhere else**, so the module's export surface is the public
 * ABI by construction rather than by remembering to add symbols to a list. It
 * expands to nothing on native builds, where the static library exports whatever
 * the linker is asked for.
 */
#if defined(__wasm__)
#define IBHA_CSVD_API __attribute__((visibility("default")))
#else
#define IBHA_CSVD_API
#endif

/* Default input ceiling, per spec 13.4. Chosen to sit at the stated real world
 * maximum so no legitimate file is rejected, and no higher because a resident
 * pair at this size costs roughly 650 MB. */
#define IBHA_CSVD_DEFAULT_MAX_BYTES ((uint64_t)150 * 1024 * 1024)

typedef enum {
    IBHA_CSVD_OK = 0,
    IBHA_CSVD_ERR_OOM = -1,
    IBHA_CSVD_ERR_IO = -2,
    IBHA_CSVD_ERR_TOO_LARGE = -3,
    IBHA_CSVD_ERR_UNTERMINATED_QUOTE = -4,
    IBHA_CSVD_ERR_RAGGED_ROW = -5,
    IBHA_CSVD_ERR_DUPLICATE_KEY = -6,
    IBHA_CSVD_ERR_COLUMN_ORDER = -7,
    IBHA_CSVD_ERR_NO_HEADER = -8,
    IBHA_CSVD_ERR_MISSING_KEY_COLUMN = -9,
    IBHA_CSVD_ERR_BAD_CONTENT = -10,
    IBHA_CSVD_ERR_INVALID_ARG = -11,
    IBHA_CSVD_ERR_UNIMPLEMENTED = -12
} ibha_csvd_status;

/* Human readable name of a status code. Never NULL, never allocates. */
IBHA_CSVD_API const char *ibha_csvd_status_name(ibha_csvd_status st);

/* Semantic version of the linked library, for bindings to assert against. */
IBHA_CSVD_API void ibha_csvd_version(int *major, int *minor, int *patch);

/* ---------------------------------------------------------------- context -- */

typedef struct ibha_csvd_ctx ibha_csvd_ctx;

typedef struct {
    /* 0 selects IBHA_CSVD_DEFAULT_MAX_BYTES. Enforced as bytes arrive, so an
     * oversized stream fails at the byte that crosses the limit rather than
     * after being buffered. */
    uint64_t max_bytes;
    uint32_t max_rows;    /* 0 = unlimited */
    uint32_t max_columns; /* 0 = unlimited */
} ibha_csvd_limits;

/* Fills limits with the documented defaults. */
IBHA_CSVD_API void ibha_csvd_limits_init(ibha_csvd_limits *out);

/* Returns NULL only if the initial arena block cannot be obtained. limits may
 * be NULL, in which case defaults apply. */
IBHA_CSVD_API ibha_csvd_ctx *ibha_csvd_ctx_new(const ibha_csvd_limits *limits);

/* Releases the context and every allocation made from its arena. Safe on NULL. */
IBHA_CSVD_API void ibha_csvd_ctx_free(ibha_csvd_ctx *ctx);

/* The first error recorded on this context, or IBHA_CSVD_OK. */
IBHA_CSVD_API ibha_csvd_status ibha_csvd_ctx_status(const ibha_csvd_ctx *ctx);

/* Human readable detail for that error, including row numbers and key values
 * where applicable. Empty string when status is OK. Owned by the context and
 * valid until it is freed. */
IBHA_CSVD_API const char *ibha_csvd_ctx_error(const ibha_csvd_ctx *ctx);

/* Bytes currently reserved from the system by this context's arena. Exposed so
 * a batch driver can size its concurrency, per spec 2.6.5. */
IBHA_CSVD_API uint64_t ibha_csvd_ctx_bytes_reserved(const ibha_csvd_ctx *ctx);

/* ----------------------------------------------------------------- ingest -- */

/*
 * Pull based byte source. Writes at most cap bytes into dst.
 *   > 0  bytes produced
 *     0  end of stream
 *   < 0  source error; the engine records IBHA_CSVD_ERR_IO
 *
 * Every real source is an adapter over this: a fixed buffer, a POSIX fd, an
 * mmapped region, a JS ReadableStream pumped from the host, an S3 GetObject
 * body, a JDBC batch, an FTP socket.
 */
typedef int64_t (*ibha_csvd_read_fn)(void *read_ctx, uint8_t *dst, size_t cap);

/* Adapter over a caller owned buffer. The buffer must outlive the read. */
typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t pos;
} ibha_csvd_buffer_reader;

IBHA_CSVD_API void ibha_csvd_buffer_reader_init(ibha_csvd_buffer_reader *r, const void *bytes, size_t len);
IBHA_CSVD_API int64_t ibha_csvd_buffer_read(void *read_ctx, uint8_t *dst, size_t cap);

#ifndef __wasm__
/* Adapter over a POSIX file descriptor. Native builds only. */
typedef struct {
    int fd;
} ibha_csvd_fd_reader;

IBHA_CSVD_API void ibha_csvd_fd_reader_init(ibha_csvd_fd_reader *r, int fd);
IBHA_CSVD_API int64_t ibha_csvd_fd_read(void *read_ctx, uint8_t *dst, size_t cap);
#endif

typedef struct {
    uint64_t bytes;        /* total bytes pulled from the source */
    uint64_t line_breaks;  /* LF count, a proxy metric only, not a record count */
    uint64_t quotes;       /* double quote count, likewise */
} ibha_csvd_ingest_stats;

/*
 * Phase 0 ingest: pulls the whole source through the reader, enforcing the
 * byte limit as it goes, and returns a byte level tally. This is deliberately
 * not a parser. It exists to measure the ceiling of the ingest path and to
 * establish the Phase 0 throughput baseline that spec section 11 gates the
 * SIMD decision on.
 *
 * Bytes are retained in the context arena so that later phases can index over
 * them without a second read. Pass retain = 0 to scan without retaining, which
 * is what the throughput benchmark uses to isolate scan cost from allocation.
 */
/*
 * size_hint is the expected total byte count, or 0 when unknown. Supplying it
 * when the source size is known (stat on a file, Content-Length, a Blob's size,
 * a buffer's length) lets the retain buffer be allocated once at the right size.
 * Without it the buffer doubles, and because the arena never reclaims the
 * abandoned copies the peak cost of retaining N bytes is roughly 4x N rather
 * than 1x. That ratio is what sets how many concurrent diffs fit on a batch
 * worker, per spec 2.6.5, so callers that know the size should always pass it.
 */
IBHA_CSVD_API ibha_csvd_status ibha_csvd_ingest(ibha_csvd_ctx *ctx, ibha_csvd_read_fn read, void *read_ctx,
                                  int retain, uint64_t size_hint, ibha_csvd_ingest_stats *out);

/* The retained bytes from the last ibha_csvd_ingest with retain != 0.
 * Returns NULL and sets *len to 0 when nothing was retained. */
IBHA_CSVD_API const uint8_t *ibha_csvd_ingest_bytes(const ibha_csvd_ctx *ctx, size_t *len);

/* ------------------------------------------------------- columnar index -- */

/* Per field flags. FIELD_HAS_ESCAPE is set only when a "" pair was actually
 * seen, because that is what selects the slow comparison path; a field that is
 * merely quoted compares with a plain memcmp. */
#define IBHA_CSVD_FIELD_QUOTED 0x01u
#define IBHA_CSVD_FIELD_HAS_ESCAPE 0x02u
#define IBHA_CSVD_FIELD_HAS_NEWLINE 0x04u
#define IBHA_CSVD_FIELD_EMPTY 0x08u

/* Per column flags, read from the header rows of the schema-authoritative file. */
#define IBHA_CSVD_COL_KEY 0x01u
#define IBHA_CSVD_COL_REQUIRED 0x02u

/* Sentinel for "this row does not exist", used for absent header rows. */
#define IBHA_CSVD_NO_ROW 0xFFFFFFFFu

/* Sentinel for "this column has no counterpart on the other side", used by the
 * column policy of spec 6.6. */
#define IBHA_CSVD_NO_COLUMN 0xFFFFFFFFu

/*
 * The columnar index, per spec 3.1. Struct of arrays over one immutable byte
 * region. Deliberately a concrete public layout rather than an opaque handle:
 * the JS and Java bindings read these arrays straight out of engine memory, and
 * an accessor call per cell would cost more than the diff.
 *
 * field_off points past the opening quote and field_len excludes the closing
 * quote, so the range is the raw region *inside* the quotes. It still contains
 * any "" pairs; the logical value is that range with "" collapsed to ", which
 * is what every comparison and hash in this library operates on.
 */
typedef struct {
    const uint8_t *bytes; /* the parsed region, owned by the context arena or borrowed */
    size_t len;

    uint32_t *field_off;   /* one per cell, row major */
    uint32_t *field_len;   /* logical extent, excludes the quotes */
    uint8_t *field_flags;  /* IBHA_CSVD_FIELD_* */
    uint32_t n_fields;

    /* Explicit rather than row * n_columns, so a ragged row stays representable
     * and is reported rather than crashing an index calculation. n_rows + 1
     * entries; the last is a sentinel equal to n_fields. */
    uint32_t *row_first_field;
    uint32_t n_rows;

    /*
     * Per row digests, filled during the parse. Zero for header rows, which are
     * not compared.
     *
     * row_key_hash and row_full_hash are computed over *normalized* values, using
     * exactly the comparators of spec 5.3, so that the unchanged-row fast path of
     * spec 6.1 step 3 agrees with a cell by cell comparison. If the comparator
     * says 1.50 equals 1.5, so do these digests.
     *
     * row_raw_hash is the same fold over the un-normalized logical values. Two
     * rows whose full hashes agree but whose raw hashes differ are equal only
     * because normalization suppressed something, which is what spec 5.3 requires
     * be counted rather than silently hidden.
     */
    uint64_t *row_key_hash;
    uint64_t *row_full_hash;
    uint64_t *row_raw_hash;

    uint32_t n_columns; /* established by the first row */
    uint8_t quote;      /* the dialect's quote byte, so a cell can be read back
                         * without the caller having to remember the dialect */

    /*
     * Digest of the comparison settings and of the schema facts the digests
     * depend on. The diff refuses two tables whose ids differ, because comparing
     * digests computed under different normalization rules would produce a
     * confidently wrong answer rather than an error.
     */
    uint64_t compare_id;

    /* 0 when the parse ran with hash_rows off, which the benchmark does. The
     * digest arrays are then all zero, and zero is a legitimate digest, so the
     * diff has to be told rather than left to guess. */
    uint8_t has_digests;
} ibha_csvd_table;

/*
 * The declared type of a column, read from header row 3 of the schema
 * authoritative file. It is not just validation metadata: per spec 5.3 it
 * selects the comparator, which is what stops a spreadsheet round trip that
 * rewrote 1.50 as 1.5 and 00123 as 123 from reporting the whole file as changed.
 */
typedef enum {
    IBHA_CSVD_TYPE_UNKNOWN = 0, /* also the absent case: trimmed byte equality */
    IBHA_CSVD_TYPE_VARCHAR,
    IBHA_CSVD_TYPE_CHAR,
    IBHA_CSVD_TYPE_DECIMAL, /* DECIMAL, NUMERIC, and the float types */
    IBHA_CSVD_TYPE_INTEGER, /* INTEGER, BIGINT, SMALLINT, TINYINT */
    IBHA_CSVD_TYPE_BOOLEAN,
    IBHA_CSVD_TYPE_DATE,
    IBHA_CSVD_TYPE_TIMESTAMP
} ibha_csvd_type;

/* Longest declared type text that is examined. Anything longer is UNKNOWN,
 * which compares as trimmed bytes and is always safe. */
#define IBHA_CSVD_TYPE_TEXT_MAX 40u

/*
 * The resolved header model. Row indices are into the table above, so the
 * declared types and column names stay in place and are never materialized.
 */
typedef struct {
    uint32_t n_columns;
    uint32_t n_key_columns;
    uint8_t *col_flags; /* n_columns entries of IBHA_CSVD_COL_* */

    /* Parsed once from type_row, because re-parsing "DECIMAL(12,2)" per cell
     * would cost more than the comparison it selects. col_size is the length or
     * the precision and col_scale the scale, both -1 when the type declares
     * none. Neither takes part in comparison; they are what a later phase's
     * validation findings are measured against. */
    uint8_t *col_type;  /* n_columns entries of ibha_csvd_type */
    int32_t *col_size;
    int32_t *col_scale;

    uint32_t key_row;      /* table row index, or IBHA_CSVD_NO_ROW */
    uint32_t required_row; /* likewise */
    uint32_t type_row;     /* likewise */
    uint32_t name_row;     /* likewise */
    uint32_t first_data_row;

    /* Set when the target inherited its schema from the source because it
     * carried only a column name row, per spec 13.8 step 3. */
    int names_only;
} ibha_csvd_schema;

/* ----------------------------------------------------------------- parse -- */

typedef struct {
    uint8_t delimiter; /* default ',' */
    uint8_t quote;     /* default '"' */
    uint8_t strip_bom; /* default 1, strips a leading UTF-8 BOM */
} ibha_csvd_dialect;

IBHA_CSVD_API void ibha_csvd_dialect_init(ibha_csvd_dialect *out);

/* Ask for target header auto-detection, per spec 13.8. Requires expect_table
 * and expect_schema to be set. */
#define IBHA_CSVD_HEADER_AUTO 0xFFFFFFFFu

/* How far auto-detection looks for the column name row. Spec 13.8 step 1. */
#define IBHA_CSVD_HEADER_SCAN_ROWS 8u

/*
 * The four header row model. Row numbers are 1 based and count header rows, so
 * the defaults describe: row 1 KEY markers, row 2 REQUIRED markers, row 3
 * declared types, row 4 column names.
 *
 * rows = 1 means a names-only file. rows = 0 means no header at all, in which
 * case there are no key columns and every row is data.
 */
typedef struct {
    uint32_t rows;          /* 4, or 1, or 0, or IBHA_CSVD_HEADER_AUTO */
    uint32_t key_row;       /* 0 = absent */
    uint32_t required_row;  /* 0 = absent */
    uint32_t type_row;      /* 0 = absent */
    uint32_t name_row;      /* 0 = absent */
} ibha_csvd_header_opts;

/* ------------------------------------------------------------ comparison -- */

typedef enum {
    /*
     * Byte equality after trim, on the canonical form. The default, because
     * guessing between 1/5/2024 and 5/1/2024 is not something a library should
     * do silently (spec 5.3).
     *
     * "Canonical form" means one thing and only one thing here: a TIMESTAMP's
     * fractional seconds carry no trailing zeros, so
     *
     *     14:22:05 == 14:22:05.000        14:22:05.100 == 14:22:05.1
     *
     * which is the rule 'numeric' already applies to DECIMAL, for the same
     * reason. A driver rendering seconds only and an export written to fixed
     * millisecond precision are describing the same instant, and calling them
     * different reports every row of a table as modified. Nothing else about the
     * text is interpreted: the date is not parsed, no format is guessed, and
     * 31/01/2026 still differs from 2026-01-31.
     */
    IBHA_CSVD_DATE_EXACT = 0,
    /* Opt in, and it requires an explicit input format list. Not implemented
     * yet: asking for it returns IBHA_CSVD_ERR_UNIMPLEMENTED rather than
     * quietly falling back to exact, which would look like it worked. */
    IBHA_CSVD_DATE_VALUE = 1
} ibha_csvd_date_compare;

/*
 * How a cell's declared type is turned into an equality test, per spec 5.3.
 *
 * These settings are baked into every row digest at parse time, so the same
 * settings must be used for the parse of both sides and for the diff over them.
 * The engine does not take that on trust: it digests them into
 * ibha_csvd_table.compare_id and refuses a mismatched pair.
 */
typedef struct {
    int trim_whitespace; /* default 1: leading and trailing spaces and tabs */
    int char_ignore_pad; /* default 1: CHAR(n) ignores trailing pad even when
                          * trim_whitespace is off */
    int numeric;         /* default 1: DECIMAL and INTEGER compare by value, so
                          * 1.50 equals 1.5 and 007 equals 7 */
    int booleans;        /* default 1: BOOLEAN compares against the truth sets */
    int date_compare;    /* ibha_csvd_date_compare */

    /* Comma separated, matched case insensitively. NULL selects
     * "TRUE,T,YES,Y,1" and "FALSE,F,NO,N,0". A value in neither set is compared
     * as trimmed bytes, so an unrecognized boolean is never silently coerced.
     * The caller owns the strings and they must outlive the parse and the diff. */
    const char *bool_true;
    const char *bool_false;

    /*
     * Spec 6.6 and 13.10. Whether a column present in one file and absent from
     * the other is an error or a finding. Both default to 0, which is the locked
     * decision of 13.10: the uploaded file must carry the same columns as the
     * source, in the same order.
     *
     * They are separate because the two cases are not equally serious. A salesman
     * appending a column is being helpful and the data you asked for is all still
     * there; a column that has gone missing is data loss, and tolerating it means
     * quietly not comparing something. So allow_added_columns = 1 with
     * allow_removed_columns = 0 is the combination most callers want.
     *
     * When a difference is allowed, the diff compares the columns the two files
     * have in common, in the source's order, and reports the rest as findings:
     * stats.columns_added, stats.columns_removed, and named entries in the
     * summary emitter's schemaFindings.
     *
     * Three things these flags never relax:
     *
     *   - **Reordering stays a hard error.** The common columns must appear in the
     *     same relative order in both files. Spec 13.10 has no flag for order and
     *     this does not add one.
     *   - **A missing KEY column is always an error**, whatever the policy,
     *     because the key is what row matching is built on.
     *   - **A file with no column name row cannot use either flag.** Without names
     *     there is no way to tell an added column from a shifted one, and guessing
     *     would silently compare the wrong pairs of cells. A count mismatch
     *     remains IBHA_CSVD_ERR_COLUMN_ORDER there.
     *
     * These live here, with the comparison settings rather than with the parse
     * options, because they change which cells the row digests are folded from.
     * That makes them part of ibha_csvd_table.compare_id, so a pair parsed under
     * different column policies is refused rather than silently compared.
     */
    int allow_added_columns;
    int allow_removed_columns;
} ibha_csvd_compare_opts;

IBHA_CSVD_API void ibha_csvd_compare_opts_init(ibha_csvd_compare_opts *out);

typedef struct {
    ibha_csvd_dialect dialect;
    ibha_csvd_header_opts header;
    ibha_csvd_compare_opts compare;

    /*
     * The source side's index and schema, when parsing a target. The source file
     * is authoritative for all schema metadata (spec 13.8), so these drive
     * header auto-detection and the column order check, and the resulting schema
     * is inherited from them rather than read out of the target.
     */
    const ibha_csvd_table *expect_table;
    const ibha_csvd_schema *expect_schema;

    /*
     * Expected total byte count, or 0 when unknown. Sizes the byte buffer and,
     * once the first rows have been seen, the index arrays. Without it those
     * arrays double and the arena does not reclaim the abandoned copies, which
     * costs roughly 2x in peak memory. See the size_hint note on ingest.
     */
    uint64_t size_hint;

    /* Compute row_key_hash and row_full_hash during the parse. Default 1. Off is
     * for the benchmark, which needs to separate state machine cost from hash
     * cost, and for callers that only want the index. */
    int hash_rows;
} ibha_csvd_parse_opts;

IBHA_CSVD_API void ibha_csvd_parse_opts_init(ibha_csvd_parse_opts *out);

typedef struct {
    uint64_t bytes;
    uint32_t n_rows;
    uint32_t n_fields;
    uint32_t n_columns;
    uint32_t quoted_fields;
    uint32_t escaped_fields;
    uint32_t multiline_fields;
    /* Rows whose only excess was empty trailing fields, normalized rather than
     * rejected, per spec 13.5. */
    uint32_t ragged_normalized;
    /* Wholly empty lines, which are skipped. A deliberately empty single column
     * row is written "" and is not affected. */
    uint32_t blank_lines;
} ibha_csvd_parse_stats;

typedef struct ibha_csvd_parser ibha_csvd_parser;

/*
 * Streaming parse. The state machine is resumable across arbitrary chunk
 * boundaries: a chunk may split a multi byte UTF-8 sequence, a quoted field, a
 * "" escape pair, a CRLF or the BOM. One shot is the degenerate case of feeding
 * a single chunk.
 *
 * Returns NULL only when the context cannot allocate. opts may be NULL.
 */
IBHA_CSVD_API ibha_csvd_parser *ibha_csvd_parse_begin(ibha_csvd_ctx *ctx, const ibha_csvd_parse_opts *opts);

/* Copies the chunk into the context arena and parses as far as it can. */
IBHA_CSVD_API ibha_csvd_status ibha_csvd_parse_chunk(ibha_csvd_parser *p, const void *bytes, size_t len);

/*
 * Zero copy variant: the whole input at once, left where the caller put it. The
 * buffer must outlive the context, because the index points into it. This is the
 * mmap and the "already have the bytes" path. Mutually exclusive with
 * ibha_csvd_parse_chunk on the same parser.
 */
IBHA_CSVD_API ibha_csvd_status ibha_csvd_parse_borrow(ibha_csvd_parser *p, const void *bytes, size_t len);

/* Completes the last record, resolves the header model and validates it. */
IBHA_CSVD_API ibha_csvd_status ibha_csvd_parse_finish(ibha_csvd_parser *p);

IBHA_CSVD_API const ibha_csvd_table *ibha_csvd_table_of(const ibha_csvd_parser *p);
IBHA_CSVD_API const ibha_csvd_schema *ibha_csvd_schema_of(const ibha_csvd_parser *p);
IBHA_CSVD_API const ibha_csvd_parse_stats *ibha_csvd_parse_stats_of(const ibha_csvd_parser *p);

/* Adapter that pulls a whole source through the reader. Streaming remains the
 * only path; this is a loop over parse_chunk. */
IBHA_CSVD_API ibha_csvd_status ibha_csvd_parse_stream(ibha_csvd_ctx *ctx, ibha_csvd_read_fn read, void *read_ctx,
                                        const ibha_csvd_parse_opts *opts, ibha_csvd_parser **out);

/* ---------------------------------------------------------------- fields -- */

/* Field index of a cell, or UINT32_MAX when the row or column is out of range. */
IBHA_CSVD_API uint32_t ibha_csvd_row_field(const ibha_csvd_table *t, uint32_t row, uint32_t col);
IBHA_CSVD_API uint32_t ibha_csvd_row_field_count(const ibha_csvd_table *t, uint32_t row);

/*
 * Compares two cells by logical value: quoting and "" escaping are collapsed as
 * the comparison walks, nothing is allocated and no string is built. Quoting is
 * therefore never a difference, per spec 5.2. Returns <0, 0 or >0 like memcmp.
 */
IBHA_CSVD_API int ibha_csvd_field_cmp(const ibha_csvd_table *ta, uint32_t fa, const ibha_csvd_table *tb,
                        uint32_t fb);

/* Compares a cell against a NUL terminated ASCII string, same rules. */
IBHA_CSVD_API int ibha_csvd_field_cmp_str(const ibha_csvd_table *t, uint32_t f, const char *s);

/* Hash of a cell's logical value. Two cells that compare equal always hash
 * equal, whatever their quoting. */
IBHA_CSVD_API uint64_t ibha_csvd_field_hash(const ibha_csvd_table *t, uint32_t f);

/* Length of the logical value, which is field_len minus one byte per "" pair. */
IBHA_CSVD_API uint32_t ibha_csvd_field_logical_len(const ibha_csvd_table *t, uint32_t f);

/*
 * Materializes the logical value into dst. This is the only sanctioned place a
 * string is built, and it exists for rendering and export boundaries. Returns
 * the logical length; writes nothing when cap is too small.
 */
IBHA_CSVD_API uint32_t ibha_csvd_field_copy(const ibha_csvd_table *t, uint32_t f, uint8_t *dst, size_t cap);

/*
 * Compares two cells under a declared type, which is the comparator spec 5.3
 * selects. With IBHA_CSVD_TYPE_UNKNOWN and default options this is
 * ibha_csvd_field_cmp with whitespace trimmed. opts may be NULL for the
 * defaults. Returns <0, 0 or >0.
 *
 * Equal here always implies equal digests, because the digests are folded from
 * the same normalized bytes this walks.
 */
IBHA_CSVD_API int ibha_csvd_field_cmp_typed(const ibha_csvd_table *ta, uint32_t fa, const ibha_csvd_table *tb,
                              uint32_t fb, ibha_csvd_type type,
                              const ibha_csvd_compare_opts *opts);

/* ------------------------------------------------------------------- diff -- */

typedef enum {
    /*
     * Spec 6.5, the default. A deleted source row is emitted immediately after
     * the target position of the most recently matched source row, so a row
     * deleted from the middle of the file appears in the middle of the report
     * next to its former neighbours. Consecutive deletions stay grouped in
     * source order.
     */
    IBHA_CSVD_DELETED_ANCHORED = 0,
    /* All deletions in a block at the end. Also what 'anchored' degrades to when
     * the caller says the source side has no meaningful order (spec 6.7). */
    IBHA_CSVD_DELETED_END = 1
} ibha_csvd_deleted_placement;

typedef struct {
    /* Must equal the settings both sides were parsed with. */
    ibha_csvd_compare_opts compare;

    /* Spec 6.2. Off reports no row as moved and skips the LIS entirely. */
    int detect_moves;

    /*
     * Spec 13.2 and 6.7. The caller's assertion that the source side has a
     * meaningful row order; a CSV file always does. When false, move detection
     * is forced off and reported as forced off in the stats rather than silently
     * ignored, and deleted rows are placed at the end.
     */
    int source_ordered;

    /* Spec 5.3. Count the cells that are equal only because normalization
     * suppressed a difference. Costs a cell walk over rows whose raw digests
     * differ but whose normalized digests agree. */
    int count_suppressed;

    /*
     * Spec 13.5's validation findings. Default 1.
     *
     * This is the one thing that makes the cursor read cells it would otherwise
     * skip: an unchanged row is decided from its digest alone, but whether its
     * cells satisfy the declared schema is not something a digest can answer. The
     * cost is bounded by a per column pre-filter, so a schema that declares no
     * REQUIRED column, no length and no numeric type costs nothing at all. Turn
     * it off for a pure diff where only the edit script matters.
     */
    int validate;

    /* Fail with IBHA_CSVD_ERR_MISSING_KEY_COLUMN when the schema declares no key
     * column, instead of taking the all-keys path of spec 6.4. */
    int require_key;

    uint8_t deleted_placement; /* ibha_csvd_deleted_placement */

    /* Spec 6.4 stage 2, the all-keys similarity pairing. The threshold is an
     * integer percentage rather than a fraction so that the result cannot depend
     * on floating point, which spec 3.2 requires be identical between the wasm32
     * and the native builds. */
    uint32_t similarity_k;       /* candidates considered per row, default 16 */
    uint32_t similarity_percent; /* default 50 */
    /* Ceiling on total candidate scoring work. Reaching it stops pairing and
     * sets pairing_truncated rather than hanging on a pathological input. */
    uint64_t max_pair_work;
} ibha_csvd_diff_opts;

IBHA_CSVD_API void ibha_csvd_diff_opts_init(ibha_csvd_diff_opts *out);

typedef enum {
    IBHA_CSVD_ROW_UNCHANGED = 0,
    IBHA_CSVD_ROW_MODIFIED = 1,
    IBHA_CSVD_ROW_ADDED = 2,
    IBHA_CSVD_ROW_DELETED = 3
} ibha_csvd_row_kind;

/*
 * Per cell flags on a report row. Several can be set at once: one column changed,
 * another differs only in formatting, a third is over its declared length.
 *
 * The first two are comparison results. The rest are the validation findings of
 * spec 13.5, which are output rather than errors: aborting on the first empty
 * REQUIRED cell would hide the other four hundred and make the feature useless.
 * They are evaluated against the *source* file's schema, which spec 13.8 makes
 * authoritative, and on the values the report row actually carries: the target
 * row where there is one, the source row for a deleted row.
 */
#define IBHA_CSVD_CELL_CHANGED 0x01u
#define IBHA_CSVD_CELL_SUPPRESSED 0x02u /* differs in bytes, equal once normalized */
#define IBHA_CSVD_CELL_REQUIRED_EMPTY 0x04u /* REQUIRED column, empty value */
#define IBHA_CSVD_CELL_TOO_LONG 0x08u       /* longer than its VARCHAR(n) or CHAR(n) */
#define IBHA_CSVD_CELL_NOT_NUMERIC 0x10u    /* does not parse as its DECIMAL or INTEGER */
#define IBHA_CSVD_CELL_PRECISION 0x20u      /* parses, but exceeds its DECIMAL(p,s) */

/* Every validation finding, for testing a cell in one mask. */
#define IBHA_CSVD_CELL_FINDING                                                            \
    (IBHA_CSVD_CELL_REQUIRED_EMPTY | IBHA_CSVD_CELL_TOO_LONG | IBHA_CSVD_CELL_NOT_NUMERIC | \
     IBHA_CSVD_CELL_PRECISION)

/*
 * One report row. Everything in it is valid until the next ibha_csvd_cursor_next
 * on the same cursor. Nothing is copied and nothing accumulates.
 *
 * Moved is a flag rather than a kind of its own, because a row can move and be
 * modified in the same edit and collapsing that would lose one of the two facts.
 */
typedef struct {
    uint8_t kind;  /* ibha_csvd_row_kind */
    uint8_t moved; /* outside the longest increasing subsequence, per spec 6.2 */
    int32_t move_distance;

    uint32_t source_row; /* table row index, IBHA_CSVD_NO_ROW when added */
    uint32_t target_row; /* table row index, IBHA_CSVD_NO_ROW when deleted */

    uint32_t n_columns;
    uint32_t n_changed_cells;
    uint32_t n_suppressed_cells;
    uint32_t n_findings; /* cells carrying at least one IBHA_CSVD_CELL_FINDING */
    /* n_columns entries of IBHA_CSVD_CELL_*, all zero unless kind is MODIFIED, or
     * suppression was found, or a validation finding fired. Points at one reused
     * buffer inside the cursor. */
    const uint8_t *cell_flags;
} ibha_csvd_row;

/*
 * Row counts are final as soon as ibha_csvd_diff_run returns: the matcher knows
 * them without looking at a single cell. Cell counts are not, because finding
 * which cells changed is exactly the work the cursor exists to defer. They
 * accumulate as the cursor advances and are final once it has been drained.
 */
typedef struct {
    uint32_t rows_unchanged;
    uint32_t rows_modified;
    uint32_t rows_added;
    uint32_t rows_deleted;
    uint32_t rows_moved;
    uint32_t report_rows; /* how many rows the cursor will yield in total */

    uint64_t cells_changed;
    uint64_t cells_suppressed; /* spec 5.3's suppressedByNormalization */

    /* Validation findings, spec 13.5. Like the cell counts these accumulate as
     * the cursor advances. The summary emitter recounts from a private drain so
     * that its numbers are always those of exactly one pass. */
    uint64_t cells_required_empty;
    uint64_t cells_too_long;
    uint64_t cells_not_numeric;
    uint64_t cells_bad_precision;
    uint32_t rows_with_findings;

    /*
     * Spec 6.6, and non zero only when compare.allow_added_columns or
     * allow_removed_columns let them be. Added counts columns in the uploaded
     * file that the source does not declare; removed counts columns the source
     * declares that the uploaded file does not carry. Both are final when
     * diff_run returns, and the summary emitter names them.
     *
     * n_columns_compared is what the report rows actually carry: the columns the
     * two files have in common, in the source's order.
     */
    uint32_t columns_added;
    uint32_t columns_removed;
    uint32_t n_columns_compared;

    uint32_t paired_by_similarity; /* spec 6.4 stage 2 */
    int pairing_truncated;         /* the work cap was reached */
    int all_keys;                  /* the spec 6.4 path was taken */
    int moves_forced_off;          /* spec 6.7, source_ordered was false */
} ibha_csvd_diff_stats;

typedef struct ibha_csvd_diff ibha_csvd_diff;
typedef struct ibha_csvd_cursor ibha_csvd_cursor;

/*
 * Matches the two sides and decides the report order. Does not compare a single
 * cell: that happens lazily in the cursor.
 *
 * Both tables must have been parsed with digests on and with the same comparison
 * settings as opts->compare. The source side is schema authoritative, so the
 * target is expected to have been parsed with expect_table and expect_schema
 * pointing at the source.
 *
 * Returns NULL and records the error on the context on failure, including a
 * duplicate key on either side (spec 13.9), which is a hard error.
 */
IBHA_CSVD_API ibha_csvd_diff *ibha_csvd_diff_run(ibha_csvd_ctx *ctx, const ibha_csvd_table *src,
                                   const ibha_csvd_schema *src_schema, const ibha_csvd_table *tgt,
                                   const ibha_csvd_schema *tgt_schema,
                                   const ibha_csvd_diff_opts *opts);

IBHA_CSVD_API const ibha_csvd_diff_stats *ibha_csvd_diff_stats_of(const ibha_csvd_diff *d);

typedef enum {
    IBHA_CSVD_SIDE_SOURCE = 0,
    IBHA_CSVD_SIDE_TARGET = 1
} ibha_csvd_side;

/*
 * The table and the schema the diff is actually comparing, which are not always
 * the ones that were parsed.
 *
 * Under the column policy of spec 6.6 each side is projected onto the columns the
 * two files have in common, in the source's order, so column c of a report row is
 * column c of *these* tables and not of the parsed ones. A consumer that reads
 * cell values out of the parsed table instead is reading the wrong column, and
 * only when the caller allowed an added or a removed column, which makes it a bug
 * that passes every test written against the default settings.
 *
 * The emitters have always used these internally. They are public because every
 * out of process consumer needs the same thing: the JS and Java bindings decode
 * cells themselves rather than paying an accessor call each, per the note on
 * ibha_csvd_table. Both return NULL only when d is NULL or side is not one of the
 * two. Valid until the context is freed.
 */
IBHA_CSVD_API const ibha_csvd_table *ibha_csvd_diff_table(const ibha_csvd_diff *d,
                                                          ibha_csvd_side side);
IBHA_CSVD_API const ibha_csvd_schema *ibha_csvd_diff_schema(const ibha_csvd_diff *d,
                                                            ibha_csvd_side side);

/* Number of columns the report rows carry, which is stats.n_columns_compared.
 * Provided so a consumer can size a buffer before it has seen a row. */
IBHA_CSVD_API uint32_t ibha_csvd_diff_columns(const ibha_csvd_diff *d);

/*
 * The single output primitive of spec 13.3. Every emitter is a loop over this.
 * Peak memory in cursor mode is the two indexes plus one row: no report index
 * array is built unless a consumer asks for random access.
 *
 * Several cursors may be open on one diff; each carries its own position and its
 * own row buffer.
 */
IBHA_CSVD_API ibha_csvd_cursor *ibha_csvd_cursor_open(ibha_csvd_diff *d);

/* Advances to the next report row. 1 on a row, 0 at end of stream, <0 on error. */
IBHA_CSVD_API int ibha_csvd_cursor_next(ibha_csvd_cursor *cur);

/* The current row, or NULL before the first next() and after the last. */
IBHA_CSVD_API const ibha_csvd_row *ibha_csvd_cursor_row(const ibha_csvd_cursor *cur);

/* Rewinds to before the first row. Cell counts in the stats are not rewound,
 * so draining a cursor twice double counts them. */
IBHA_CSVD_API void ibha_csvd_cursor_reset(ibha_csvd_cursor *cur);

/* ------------------------------------------------------- cell level diff -- */

/*
 * Spec 7. Highlighting a whole cell loses the information the reviewer needs
 * when "Accident violation code" becomes "Accident Violation code(s)".
 *
 * Three things keep this cheap and all three are in the API rather than implied:
 * it is computed per cell on request and never in bulk, it is capped, and it is
 * returned as compact triples rather than as objects or as pre-built markup. The
 * view decides the markup.
 */
typedef enum {
    IBHA_CSVD_CELLDIFF_NONE = 0, /* the cell is changed or it is not */
    IBHA_CSVD_CELLDIFF_WORD = 1, /* tokens split on whitespace and punctuation */
    IBHA_CSVD_CELLDIFF_CHARACTER = 2, /* bytes, never splitting a UTF-8 sequence */
    /* Word first, then a character refinement inside each replaced token run.
     * This is what GitHub's intra line highlighting does and it reads better than
     * either level alone. */
    IBHA_CSVD_CELLDIFF_WORD_THEN_CHARACTER = 3
} ibha_csvd_cell_diff_mode;

typedef enum {
    IBHA_CSVD_SEG_EQUAL = 0,
    IBHA_CSVD_SEG_DELETE = 1, /* in the source value only */
    IBHA_CSVD_SEG_INSERT = 2  /* in the target value only */
} ibha_csvd_seg_op;

/*
 * One segment. Offsets are into the *logical*, unescaped value: EQUAL and DELETE
 * index the source value, INSERT indexes the target value. Three uint32_t and no
 * padding, so a binding can view an array of these as a plain Uint32Array of
 * [op, start, len] triples, which is what spec 7 asks for.
 *
 * These are byte offsets. A helper converting them to UTF-16 code unit offsets
 * for DOM ranges belongs with the view, and getting that boundary wrong is a
 * classic source of mangled non ASCII text, so the unit is stated here rather
 * than implied.
 */
typedef struct {
    uint32_t op; /* ibha_csvd_seg_op */
    uint32_t start;
    uint32_t len;
} ibha_csvd_segment;

/* Cells at or above this many logical bytes skip refinement and report as wholly
 * replaced, per spec 7 point 2. Myers is O(ND), which is fine for short strings
 * and quadratic-ish for long dissimilar ones. */
#define IBHA_CSVD_DEFAULT_MAX_CELL_BYTES 4096u

/*
 * Segments for one column of one report row. row must be the live row of a
 * cursor on d.
 *
 * Returns the total number of segments and writes the first cap of them, so a
 * caller can render a prefix or size a buffer and call again. The count does not
 * depend on cap. Returns 0 when the values are equal, when either side is absent
 * (an added or deleted row has nothing to compare against), or when mode is NONE.
 * Returns a negative ibha_csvd_status on a bad argument.
 *
 * max_bytes 0 selects IBHA_CSVD_DEFAULT_MAX_CELL_BYTES. Over the cap the result
 * is one DELETE of the whole source value plus one INSERT of the whole target
 * value, which is the same information a mode of NONE carries.
 *
 * Uses a scratch buffer owned by the diff, allocated on first use and reused
 * afterwards, so nothing accumulates per cell and two calls on the same diff must
 * not run concurrently. The engine is thread agnostic per spec 2.6.3:
 * parallelism is across diffs, not inside one.
 */
IBHA_CSVD_API int ibha_csvd_cell_segments(ibha_csvd_diff *d, const ibha_csvd_row *row, uint32_t col,
                            ibha_csvd_cell_diff_mode mode, uint32_t max_bytes,
                            ibha_csvd_segment *out, uint32_t cap);

/* -------------------------------------------------------------- emitters -- */

/*
 * The row shape the emitters and their consumers agree on, per spec 13.3. It is
 * a versioned public contract carried on every JSONL row, every CSV row and the
 * HTML container, because the emitter and the consumer are separate components
 * that must agree. Changing a shape is a breaking change and bumps this.
 */
#define IBHA_CSVD_SCHEMA_VERSION 1

/*
 * Push based byte sink, the mirror of ibha_csvd_read_fn. Returns 0 on success and
 * < 0 to abort, which surfaces as IBHA_CSVD_ERR_IO. Every emitter is a loop over
 * the cursor writing into one of these, so the library never decides the
 * destination and never needs the whole diff in memory to reach one.
 */
typedef int (*ibha_csvd_write_fn)(void *sink_ctx, const void *bytes, size_t len);

typedef struct {
    ibha_csvd_write_fn write;
    void *ctx;
} ibha_csvd_sink;

/* Adapter over a caller owned buffer. Sets overflow and keeps counting rather
 * than truncating silently, so len is always the size the output would have. */
typedef struct {
    uint8_t *bytes;
    size_t cap;
    size_t len;
    int overflow;
} ibha_csvd_buffer_sink;

IBHA_CSVD_API void ibha_csvd_buffer_sink_init(ibha_csvd_buffer_sink *s, void *bytes, size_t cap);
IBHA_CSVD_API int ibha_csvd_buffer_sink_write(void *sink_ctx, const void *bytes, size_t len);

/*
 * Fills sink with { ibha_csvd_buffer_sink_write, s }. A C caller can write those
 * two fields itself and has no need of this; a host that is not C cannot, and
 * that is what it is for.
 *
 * On wasm32 a function pointer is an index into the module's indirect call
 * table, the table is not exported, and JavaScript has no portable way to place
 * a host function into one. So without this the whole emitter layer is
 * unreachable from the binding: every symbol it needs is exported and the one
 * value that ties them together cannot be constructed. Taking the address here
 * is also what puts the write function into that table in the first place.
 */
IBHA_CSVD_API void ibha_csvd_buffer_sink_bind(ibha_csvd_sink *sink, ibha_csvd_buffer_sink *s);

#ifndef __wasm__
/* Adapter over a POSIX file descriptor. Native builds only. */
typedef struct {
    int fd;
} ibha_csvd_fd_sink;

IBHA_CSVD_API void ibha_csvd_fd_sink_init(ibha_csvd_fd_sink *s, int fd);
IBHA_CSVD_API int ibha_csvd_fd_sink_write(void *sink_ctx, const void *bytes, size_t len);
#endif

typedef enum {
    /* One JSON object per report row. The default machine format: it streams,
     * appends, greps and splits. */
    IBHA_CSVD_EMIT_JSONL = 0,
    /* A CSV diff report, for loading back into a spreadsheet or a table. */
    IBHA_CSVD_EMIT_CSV = 1,
    /* A <div> containing a <table> with the standard classes. For bounded output:
     * reports, changes-only, a page at a time. A 90,000 row diff rendered to one
     * HTML string is tens of megabytes of DOM and will not scroll acceptably;
     * that is what the virtualized view of spec section 8 is for. Both consume
     * this same cursor, so they agree by construction. */
    IBHA_CSVD_EMIT_HTML = 2,
    /* Counts, schema findings and validation findings as one JSON object.
     * Constant memory whatever the diff size. Pass/fail and email bodies. */
    IBHA_CSVD_EMIT_SUMMARY = 3
} ibha_csvd_emit_format;

typedef struct {
    uint8_t format; /* ibha_csvd_emit_format */

    /* Skip rows that are unchanged, unmoved, and carry neither a suppressed cell
     * nor a validation finding. A finding on an otherwise unchanged row is the
     * point of the run, so it is never what this drops. */
    int changes_only;

    /* Emit cell values. Off gives the edit script alone, which is what a
     * pass/fail check or a row count wants and costs nothing to write. */
    int include_values;

    /* ibha_csvd_cell_diff_mode. HTML only: the other formats carry values, not
     * markup, and a consumer that wants segments calls the segment API. */
    uint8_t cell_diff;

    /* Truncate an emitted value at this many logical bytes, backing off to a
     * UTF-8 boundary, and mark the cell truncated. 0 emits values whole. */
    uint32_t max_cell_bytes;

    /* Stop after this many report rows. 0 is unlimited. This is how the HTML
     * emitter is kept to bounded output. */
    uint32_t max_rows;

    /*
     * CSV only. A value opening with '=', '+', '@', a tab or a CR is a formula to
     * Excel, so a diff report of untrusted data is a script delivery mechanism
     * unless something intervenes. On by default: such a value is prefixed with a
     * single quote, Excel's own text marker. A leading '-' is guarded only when
     * the value is not a plain number, so ordinary negative numbers are untouched.
     */
    int csv_formula_guard;

    /* CSV only. The output delimiter, default ','. */
    uint8_t csv_delimiter;

    /*
     * HTML only. Prefix of every emitted class name, default "ibha-csvd-" per
     * spec 13.0. Validated against [A-Za-z][A-Za-z0-9_-]{0,31} and refused with
     * IBHA_CSVD_ERR_INVALID_ARG otherwise: the class attribute is the one place
     * caller data reaches the markup, and the suffixes come from a fixed compiled
     * in set. Nothing else the caller supplies is ever interpolated into an
     * attribute name, an attribute value or a URL.
     */
    const char *class_prefix;
} ibha_csvd_emit_opts;

IBHA_CSVD_API void ibha_csvd_emit_opts_init(ibha_csvd_emit_opts *out, ibha_csvd_emit_format format);

/*
 * Drains the diff into the sink in the requested format. Opens its own cursor, so
 * any cursor the caller already holds keeps its position.
 *
 * Nothing accumulates: the emitter's whole memory is one output buffer and one
 * report row, whatever the size of the diff. rows_written may be NULL, and
 * receives the number of report rows actually emitted, which differs from
 * stats.report_rows under changes_only or max_rows.
 *
 * The summary emitter zeroes the cell level counters before its own drain, so its
 * numbers are always those of exactly one pass even when the caller has already
 * drained a cursor.
 */
IBHA_CSVD_API ibha_csvd_status ibha_csvd_emit(ibha_csvd_diff *d, const ibha_csvd_emit_opts *opts,
                                const ibha_csvd_sink *sink, uint32_t *rows_written);

#ifdef __cplusplus
}
#endif

#endif /* IBHA_CSVDIFF_H */
