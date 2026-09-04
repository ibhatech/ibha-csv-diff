/*
 * parse.c - the resumable RFC 4180 state machine and the columnar index.
 *
 * Two properties drive the whole design.
 *
 * **Resumability is structural, not an optimization.** Spec 2.5 makes a
 * ReadableStream the primary input, so a chunk boundary may fall anywhere: in
 * the middle of a multi byte UTF-8 sequence, inside a quoted field, between the
 * two halves of a "" escape pair, between the CR and the LF of a CRLF, or inside
 * the BOM. The parser therefore carries its state plus one field offset across
 * calls and never looks backwards past the current field.
 *
 * **Bytes accumulate contiguously, and the index stores offsets into them.**
 * That is what makes resumability cheap: a field split across two chunks needs
 * no partial-field buffer, because by the time the field ends both halves are
 * adjacent in the byte buffer. The buffer may move when it grows, which is
 * exactly why the index holds offsets rather than pointers (spec 3.2).
 *
 * The state machine is table driven: a 256 entry byte class table built from the
 * dialect, and a (state, class) transition table carrying the next state plus an
 * action bitmask. Runs of ordinary bytes, which are the overwhelming majority,
 * are consumed by a tight skip loop rather than one table step each. The tables
 * remain the definition of the behaviour; the skip loops only shortcut the
 * transitions that do nothing.
 */
#include "internal.h"

/* ------------------------------------------------------------- machinery -- */

enum {
    S_FIELD_START = 0, /* nothing of the current field consumed yet */
    S_UNQUOTED,        /* inside a bare field */
    S_QUOTED,          /* inside a quoted field; newlines are data here */
    S_QUOTE_IN_QUOTED, /* saw a quote inside a quoted field; the next byte decides */
    S_AFTER_QUOTED,    /* field closed, skipping padding before the delimiter */
    S_AFTER_CR,        /* a bare CR ended the record; a following LF belongs to it */
    S_COUNT
};

enum { C_OTHER = 0, C_DELIM, C_QUOTE, C_CR, C_LF, C_SPACE, C_COUNT };

/* Action bits. CLOSE0 ends the field at the current byte, CLOSE1 one byte
 * earlier because the current byte follows a closing quote. Both also reset the
 * next field's start, which is what keeps the invariant that field_start equals
 * the cursor whenever the state is S_FIELD_START. */
#define A_SETSTART 0x01u
#define A_OPEN_QUO 0x02u
#define A_CLOSE0 0x04u
#define A_CLOSE1 0x08u
#define A_ROW 0x10u
#define A_ESC 0x20u
#define A_NL 0x40u
#define A_ERR 0x80u

#define TR(next, act) ((uint16_t)(((uint16_t)(next) << 8) | (uint16_t)(act)))
#define TR_NEXT(t) ((uint8_t)((t) >> 8))
#define TR_ACT(t) ((uint8_t)((t)&0xFFu))

/*
 * The grammar, in one table.
 *
 * Two permissive choices are worth naming, because both are places where real
 * files disagree with RFC 4180 and the choice decides whether a legitimate file
 * is rejected or a broken one silently misread.
 *
 * A quote inside a bare field, as in `say "hi",b`, is data. Rejecting it would
 * fail on files no spreadsheet has trouble with.
 *
 * Padding between a closing quote and the delimiter, as in `"ab" ,c`, is skipped
 * and that is the entire reason S_AFTER_QUOTED exists. Anything else after a
 * closing quote is a structural error rather than being silently dropped: the
 * alternative is to discard bytes the user can see in their file, which is the
 * worst failure mode a diff tool can have.
 */
static const uint16_t k_trans[S_COUNT][C_COUNT] = {
    /* S_FIELD_START */
    {
        TR(S_UNQUOTED, 0),
        TR(S_FIELD_START, A_CLOSE0),
        TR(S_QUOTED, A_OPEN_QUO),
        TR(S_AFTER_CR, A_CLOSE0 | A_ROW),
        TR(S_FIELD_START, A_CLOSE0 | A_ROW),
        TR(S_UNQUOTED, 0),
    },
    /* S_UNQUOTED */
    {
        TR(S_UNQUOTED, 0),
        TR(S_FIELD_START, A_CLOSE0),
        TR(S_UNQUOTED, 0),
        TR(S_AFTER_CR, A_CLOSE0 | A_ROW),
        TR(S_FIELD_START, A_CLOSE0 | A_ROW),
        TR(S_UNQUOTED, 0),
    },
    /* S_QUOTED */
    {
        TR(S_QUOTED, 0),
        TR(S_QUOTED, 0),
        TR(S_QUOTE_IN_QUOTED, 0),
        TR(S_QUOTED, A_NL),
        TR(S_QUOTED, A_NL),
        TR(S_QUOTED, 0),
    },
    /* S_QUOTE_IN_QUOTED */
    {
        TR(S_AFTER_QUOTED, A_ERR),
        TR(S_FIELD_START, A_CLOSE1),
        TR(S_QUOTED, A_ESC),
        TR(S_AFTER_CR, A_CLOSE1 | A_ROW),
        TR(S_FIELD_START, A_CLOSE1 | A_ROW),
        TR(S_AFTER_QUOTED, A_CLOSE1),
    },
    /* S_AFTER_QUOTED */
    {
        TR(S_AFTER_QUOTED, A_ERR),
        TR(S_FIELD_START, A_SETSTART),
        TR(S_AFTER_QUOTED, A_ERR),
        TR(S_AFTER_CR, A_ROW),
        TR(S_FIELD_START, A_ROW | A_SETSTART),
        TR(S_AFTER_QUOTED, 0),
    },
    /* S_AFTER_CR is resolved before the table is consulted, because it may have
     * to leave its byte unconsumed for S_FIELD_START to reprocess. */
    {
        TR(S_FIELD_START, 0),
        TR(S_FIELD_START, 0),
        TR(S_FIELD_START, 0),
        TR(S_FIELD_START, 0),
        TR(S_FIELD_START, 0),
        TR(S_FIELD_START, 0),
    },
};

struct ibha_csvd_parser {
    ibha_csvd_ctx *ctx;
    ibha_csvd_parse_opts opts;
    ibha_csvd_table tbl;
    ibha_csvd_schema schema;
    ibha_csvd_parse_stats stats;

    ibha_bytebuf buf; /* owned bytes; unused when borrowing */
    int borrowed;
    int finished;

    uint8_t cls[256];
    uint8_t skip_unquoted[256]; /* 1 when the byte cannot end a bare field */
    uint8_t skip_quoted[256];   /* 1 when the byte cannot end or flag a quoted field */

    size_t pos;
    uint8_t state;
    uint8_t field_flags;
    uint32_t field_start;
    uint32_t row_first; /* index of the open row's first field */

    /* Where the open quoted field started, so an unterminated quote can name a
     * position instead of just saying the file ended badly. */
    uint32_t quote_row;
    uint32_t quote_col;

    int bom_pending;
    int schema_resolved;
    int sized;
    uint32_t header_rows; /* IBHA_CSVD_HEADER_AUTO until detection succeeds */

    uint32_t field_cap;
    uint32_t row_cap;
};

/* ------------------------------------------------------------- defaults -- */

void ibha_csvd_dialect_init(ibha_csvd_dialect *out) {
    if (!out) return;
    out->delimiter = ',';
    out->quote = '"';
    out->strip_bom = 1;
}

void ibha_csvd_parse_opts_init(ibha_csvd_parse_opts *out) {
    if (!out) return;
    ibha_csvd_dialect_init(&out->dialect);
    ibha_csvd_compare_opts_init(&out->compare);
    out->header.rows = 4;
    out->header.key_row = 1;
    out->header.required_row = 2;
    out->header.type_row = 3;
    out->header.name_row = 4;
    out->expect_table = NULL;
    out->expect_schema = NULL;
    out->size_hint = 0;
    out->hash_rows = 1;
}

/* --------------------------------------------------------- index growth -- */

/*
 * The index arrays double, and the arena never reclaims the copies they leave
 * behind, so a doubling from nothing costs about 2x the final size in peak
 * memory. Since spec 2.6.5 makes per worker memory the thing that sets batch
 * concurrency, that is worth avoiding: once a few hundred rows have been seen we
 * can extrapolate the final count from the size hint and reserve once. The
 * doubling path stays as the fallback for a wrong or absent hint, where a bad
 * guess then costs memory rather than correctness.
 */
static int fields_reserve(ibha_csvd_parser *p, uint32_t need, int exact) {
    if (need <= p->field_cap) return 1;

    /* Doubling is right when growing one field at a time, and wrong when the
     * final size has just been estimated: rounding 2.1 million cells up to the
     * next power of two would reserve 4.2 million and waste a whole index. */
    uint32_t cap = need;
    if (!exact) {
        cap = p->field_cap ? p->field_cap : 8192u;
        while (cap < need) {
            if (cap > 0x7FFFFFFFu) {
                cap = need;
                break;
            }
            cap *= 2;
        }
    }

    uint32_t *off = (uint32_t *)ibha_arena_alloc_large(&p->ctx->arena, (size_t)cap * 4);
    uint32_t *len = (uint32_t *)ibha_arena_alloc_large(&p->ctx->arena, (size_t)cap * 4);
    uint8_t *flags = (uint8_t *)ibha_arena_alloc_large(&p->ctx->arena, (size_t)cap);
    if (!off || !len || !flags) return 0;

    if (p->tbl.n_fields) {
        IBHA_MEMCPY(off, p->tbl.field_off, (size_t)p->tbl.n_fields * 4);
        IBHA_MEMCPY(len, p->tbl.field_len, (size_t)p->tbl.n_fields * 4);
        IBHA_MEMCPY(flags, p->tbl.field_flags, (size_t)p->tbl.n_fields);
    }
    p->tbl.field_off = off;
    p->tbl.field_len = len;
    p->tbl.field_flags = flags;
    p->field_cap = cap;
    return 1;
}

static int rows_reserve(ibha_csvd_parser *p, uint32_t need, int exact) {
    if (need <= p->row_cap) return 1;

    uint32_t cap = need;
    if (!exact) {
        cap = p->row_cap ? p->row_cap : 512u;
        while (cap < need) {
            if (cap > 0x7FFFFFFFu) {
                cap = need;
                break;
            }
            cap *= 2;
        }
    }

    uint32_t *first = (uint32_t *)ibha_arena_alloc_large(&p->ctx->arena, (size_t)cap * 4);
    uint64_t *kh = (uint64_t *)ibha_arena_alloc_large(&p->ctx->arena, (size_t)cap * 8);
    uint64_t *fh = (uint64_t *)ibha_arena_alloc_large(&p->ctx->arena, (size_t)cap * 8);
    uint64_t *rh = (uint64_t *)ibha_arena_alloc_large(&p->ctx->arena, (size_t)cap * 8);
    if (!first || !kh || !fh || !rh) return 0;

    if (p->tbl.n_rows) {
        IBHA_MEMCPY(first, p->tbl.row_first_field, (size_t)(p->tbl.n_rows + 1) * 4);
        IBHA_MEMCPY(kh, p->tbl.row_key_hash, (size_t)p->tbl.n_rows * 8);
        IBHA_MEMCPY(fh, p->tbl.row_full_hash, (size_t)p->tbl.n_rows * 8);
        IBHA_MEMCPY(rh, p->tbl.row_raw_hash, (size_t)p->tbl.n_rows * 8);
    }
    p->tbl.row_first_field = first;
    p->tbl.row_key_hash = kh;
    p->tbl.row_full_hash = fh;
    p->tbl.row_raw_hash = rh;
    p->row_cap = cap;
    return 1;
}

/* Extrapolates the final row and field counts from what has been parsed so far,
 * and reserves once. Called after enough rows to make the average meaningful. */
static void size_index_from_hint(ibha_csvd_parser *p) {
    if (p->sized || p->opts.size_hint == 0 || p->pos == 0) return;
    p->sized = 1;

    uint64_t seen = (uint64_t)p->pos;
    uint64_t total = p->opts.size_hint;
    if (total <= seen) return;

    /* 3% of headroom so a slightly narrower tail does not trigger a doubling
     * that copies the whole index. */
    uint64_t est_rows = ((uint64_t)p->tbl.n_rows * total) / seen;
    est_rows += est_rows / 32 + 16;
    uint64_t est_fields = est_rows * (p->tbl.n_columns ? p->tbl.n_columns : 1u);

    if (est_rows > 0xFFFFFFF0ull || est_fields > 0xFFFFFFF0ull) return;
    (void)rows_reserve(p, (uint32_t)est_rows, 1);
    (void)fields_reserve(p, (uint32_t)est_fields, 1);
}

/* ------------------------------------------------------------- emitting -- */

static ibha_csvd_status field_push(ibha_csvd_parser *p, uint32_t off, uint32_t len, uint8_t flags) {
    if (!fields_reserve(p, p->tbl.n_fields + 1, 0)) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_OOM, "parse: cannot grow the field index past %u",
                        p->field_cap);
    }
    if (len == 0) flags |= IBHA_CSVD_FIELD_EMPTY;

    p->tbl.field_off[p->tbl.n_fields] = off;
    p->tbl.field_len[p->tbl.n_fields] = len;
    p->tbl.field_flags[p->tbl.n_fields] = flags;
    p->tbl.n_fields++;

    if (flags & IBHA_CSVD_FIELD_QUOTED) p->stats.quoted_fields++;
    if (flags & IBHA_CSVD_FIELD_HAS_ESCAPE) p->stats.escaped_fields++;
    if (flags & IBHA_CSVD_FIELD_HAS_NEWLINE) p->stats.multiline_fields++;
    return IBHA_CSVD_OK;
}

/*
 * The row digests, folded from *normalized* values.
 *
 * This is the single most important coupling in the engine. Spec 6.1 step 3 says
 * that a matched row whose full digests agree is unchanged and that its cells are
 * never compared. That is only sound if the digest means exactly what the
 * comparator means, so both go through ibha_normalize and neither has its own
 * idea of equality. If they diverged, a row whose only edit was 1.50 becoming
 * 1.5 would be called modified by the fast path and then found identical by the
 * cell walk that follows it.
 *
 * row_raw_hash is the same fold over un-normalized values, and it is what makes
 * spec 5.3's suppressedByNormalization countable: equal full hashes with unequal
 * raw hashes means "these rows are equal only because normalization suppressed
 * something", which the cursor then walks to find. Cells that normalization did
 * not touch, which is nearly all of them, reuse the hash already computed rather
 * than being hashed a second time.
 */
static void hash_row(ibha_csvd_parser *p, uint32_t row) {
    /* The fold itself lives in normalize.c, next to the comparators it has to
     * agree with, because the column projection of spec 6.6 has to redo exactly
     * this over a different set of columns and two implementations of it would be
     * two chances to disagree with ibha_norm_cmp. */
    ibha_hash_row(&p->tbl, &p->schema, &p->opts.compare, row);
}

/* Reports a ragged row with the numbers a user needs to find it. */
static ibha_csvd_status ragged_err(ibha_csvd_parser *p, uint32_t got, uint32_t want) {
    return ibha_err(p->ctx, IBHA_CSVD_ERR_RAGGED_ROW,
                    "row %llu has %u fields, expected %u", (uint64_t)p->tbl.n_rows + 1, got, want);
}

/*
 * Completes a record: ragged handling, then the header model, then the digests.
 *
 * Ragged rows are an error, with one exception from spec 13.5: a row whose only
 * excess is empty trailing fields is normalized rather than rejected, because
 * Excel emits trailing empty columns routinely and rejecting those files would
 * be a support burden for no benefit. A row with too few fields is not
 * normalized: missing data means the file is broken, and padding it would hide
 * exactly the corruption a diff is supposed to surface.
 */
static ibha_csvd_status row_end(ibha_csvd_parser *p) {
    uint32_t nf = p->tbl.n_fields - p->row_first;

    if (p->tbl.n_rows == 0) {
        p->tbl.n_columns = nf;
        if (p->ctx->limits.max_columns && nf > p->ctx->limits.max_columns) {
            return ibha_err(p->ctx, IBHA_CSVD_ERR_INVALID_ARG,
                            "row 1 has %u columns, over the configured maximum of %u", nf,
                            p->ctx->limits.max_columns);
        }
    } else if (nf != p->tbl.n_columns) {
        if (nf < p->tbl.n_columns) return ragged_err(p, nf, p->tbl.n_columns);

        for (uint32_t i = p->row_first + p->tbl.n_columns; i < p->tbl.n_fields; i++) {
            if (p->tbl.field_len[i] != 0) return ragged_err(p, nf, p->tbl.n_columns);
        }
        p->tbl.n_fields = p->row_first + p->tbl.n_columns;
        p->stats.ragged_normalized++;
    }

    if (p->ctx->limits.max_rows && p->tbl.n_rows >= p->ctx->limits.max_rows) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_INVALID_ARG,
                        "file has more than the configured maximum of %u rows",
                        p->ctx->limits.max_rows);
    }

    if (!rows_reserve(p, p->tbl.n_rows + 2, 0)) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_OOM, "parse: cannot grow the row index past %u",
                        p->row_cap);
    }

    uint32_t row = p->tbl.n_rows;
    p->tbl.row_first_field[row] = p->row_first;
    p->tbl.row_key_hash[row] = 0;
    p->tbl.row_full_hash[row] = 0;
    p->tbl.row_raw_hash[row] = 0;
    p->tbl.n_rows = row + 1;
    p->tbl.row_first_field[p->tbl.n_rows] = p->tbl.n_fields; /* running sentinel */
    p->row_first = p->tbl.n_fields;

    if (!p->schema_resolved) {
        if (p->header_rows == IBHA_CSVD_HEADER_AUTO) {
            /* Spec 13.8: find the row whose cells are the source's column names.
             * Testing each row as it completes stops the scan at the first
             * match, which is by definition the column name row. */
            if (ibha_schema_row_matches_names(&p->tbl, row, p->opts.expect_table,
                                              p->opts.expect_schema, &p->opts.compare)) {
                p->header_rows = row + 1;
            } else if (p->tbl.n_rows >= IBHA_CSVD_HEADER_SCAN_ROWS) {
                return ibha_schema_resolve(p->ctx, &p->tbl, &p->opts.header, p->opts.expect_table,
                                           p->opts.expect_schema, IBHA_CSVD_HEADER_AUTO,
                                           &p->opts.compare, &p->schema);
            }
        }
        if (p->header_rows != IBHA_CSVD_HEADER_AUTO && p->tbl.n_rows >= p->header_rows) {
            ibha_csvd_status st =
                ibha_schema_resolve(p->ctx, &p->tbl, &p->opts.header, p->opts.expect_table,
                                    p->opts.expect_schema, p->header_rows, &p->opts.compare,
                                    &p->schema);
            if (st != IBHA_CSVD_OK) return st;
            p->schema_resolved = 1;
        }
    }

    /* The schema is always resolved before the first data row completes, because
     * the column name row is the last header row in every supported layout. */
    if (p->schema_resolved && row >= p->schema.first_data_row && p->opts.hash_rows) {
        hash_row(p, row);
    }

    /* 256 rows is enough for the average row width to be stable, and is reached
     * before the default index capacity forces a doubling on a typical file. */
    if (!p->sized && p->tbl.n_rows == 256) size_index_from_hint(p);
    return IBHA_CSVD_OK;
}

/* --------------------------------------------------------------- driver -- */

static ibha_csvd_status bad_content(ibha_csvd_parser *p, uint8_t byte) {
    uint32_t col = p->tbl.n_fields - p->row_first + 1;
    return ibha_err(p->ctx, IBHA_CSVD_ERR_BAD_CONTENT,
                    "unexpected byte %u after a closing quote at row %llu, field %u",
                    (unsigned int)byte, (uint64_t)p->tbl.n_rows + 1, col);
}

/*
 * Consumes everything currently available. Returns with state carried so the
 * next chunk resumes exactly where this one stopped.
 */
static ibha_csvd_status run(ibha_csvd_parser *p) {
    const uint8_t *b = p->tbl.bytes;
    size_t end = p->tbl.len;
    size_t pos = p->pos;
    uint8_t state = p->state;

    /* A BOM can be split across chunks like anything else, so it is only decided
     * once three bytes exist or the stream has ended. */
    if (p->bom_pending) {
        if (end < 3) {
            static const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
            for (size_t i = 0; i < end; i++) {
                if (b[i] != bom[i]) {
                    p->bom_pending = 0;
                    break;
                }
            }
            if (p->bom_pending) return IBHA_CSVD_OK; /* still could be a BOM */
        } else {
            p->bom_pending = 0;
            if (b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
                pos = 3;
                p->field_start = 3;
            }
        }
    }

    for (;;) {
        if (state == S_UNQUOTED) {
            while (pos < end && p->skip_unquoted[b[pos]]) pos++;
        } else if (state == S_QUOTED) {
            while (pos < end && p->skip_quoted[b[pos]]) pos++;
        } else if (state == S_AFTER_CR) {
            if (pos >= end) break;
            /* The LF of a CRLF, possibly the first byte of a later chunk. Any
             * other byte belongs to the next record and is left for it. */
            if (p->cls[b[pos]] == C_LF) pos++;
            state = S_FIELD_START;
            p->field_start = (uint32_t)pos;
            continue;
        }
        if (pos >= end) break;

        uint16_t tr = k_trans[state][p->cls[b[pos]]];
        uint8_t act = TR_ACT(tr);
        state = TR_NEXT(tr);

        if (act == 0) {
            pos++;
            continue;
        }

        if (act & A_ERR) {
            p->pos = pos;
            p->state = state;
            return bad_content(p, b[pos]);
        }

        if (act & A_ESC) p->field_flags |= IBHA_CSVD_FIELD_HAS_ESCAPE;
        if (act & A_NL) p->field_flags |= IBHA_CSVD_FIELD_HAS_NEWLINE;

        if (act & A_OPEN_QUO) {
            p->field_start = (uint32_t)pos + 1;
            p->field_flags |= IBHA_CSVD_FIELD_QUOTED;
            p->quote_row = p->tbl.n_rows + 1;
            p->quote_col = p->tbl.n_fields - p->row_first + 1;
        }

        if (act & (A_CLOSE0 | A_CLOSE1)) {
            uint32_t fend = (act & A_CLOSE1) ? (uint32_t)pos - 1 : (uint32_t)pos;
            uint32_t flen = fend - p->field_start;

            /* A wholly empty line is not a record. Writing "" instead keeps a
             * deliberately empty single column row, so nothing is lost. */
            int blank = (act & A_ROW) && flen == 0 && p->tbl.n_fields == p->row_first &&
                        p->field_flags == 0;
            if (blank) {
                p->stats.blank_lines++;
                act &= (uint8_t)~A_ROW;
            } else {
                ibha_csvd_status st = field_push(p, p->field_start, flen, p->field_flags);
                if (st != IBHA_CSVD_OK) {
                    p->pos = pos;
                    p->state = state;
                    return st;
                }
            }
            p->field_flags = 0;
        }

        if (act & (A_CLOSE0 | A_CLOSE1 | A_SETSTART)) p->field_start = (uint32_t)pos + 1;

        if (act & A_ROW) {
            /* row_end reads the cursor to extrapolate index sizes and may
             * reallocate the index arrays, so nothing about them is cached
             * across this call. The byte buffer cannot move here. */
            p->pos = pos;
            p->state = state;
            ibha_csvd_status st = row_end(p);
            if (st != IBHA_CSVD_OK) return st;
        }
        pos++;
    }

    p->pos = pos;
    p->state = state;
    return IBHA_CSVD_OK;
}

/* ------------------------------------------------------------ lifecycle -- */

static void build_tables(ibha_csvd_parser *p) {
    for (int i = 0; i < 256; i++) p->cls[i] = C_OTHER;
    p->cls[' '] = C_SPACE;
    p->cls['\t'] = C_SPACE;
    p->cls['\r'] = C_CR;
    p->cls['\n'] = C_LF;
    /* Assigned last so a tab delimited dialect still classifies its tab as the
     * delimiter rather than as padding. */
    p->cls[p->opts.dialect.quote] = C_QUOTE;
    p->cls[p->opts.dialect.delimiter] = C_DELIM;

    for (int i = 0; i < 256; i++) {
        uint8_t c = p->cls[i];
        p->skip_unquoted[i] = (uint8_t)(c == C_OTHER || c == C_SPACE);
        p->skip_quoted[i] = (uint8_t)(c == C_OTHER || c == C_SPACE || c == C_DELIM);
    }
}

ibha_csvd_parser *ibha_csvd_parse_begin(ibha_csvd_ctx *ctx, const ibha_csvd_parse_opts *opts) {
    if (!ctx) return NULL;
    if (ctx->status != IBHA_CSVD_OK) return NULL;

    ibha_csvd_parser *p = (ibha_csvd_parser *)ibha_arena_calloc(&ctx->arena, sizeof(*p));
    if (!p) {
        ibha_err(ctx, IBHA_CSVD_ERR_OOM, "parse: cannot allocate the parser");
        return NULL;
    }
    p->ctx = ctx;
    if (opts) {
        p->opts = *opts;
    } else {
        ibha_csvd_parse_opts_init(&p->opts);
    }
    if (p->opts.dialect.delimiter == 0) p->opts.dialect.delimiter = ',';
    if (p->opts.dialect.quote == 0) p->opts.dialect.quote = '"';
    /* Resolved once, here, so that the digests and every later comparison see one
     * canonical form of the settings and compare_id is reproducible. */
    ibha_compare_opts_resolve(&p->opts.compare);

    if (p->opts.compare.date_compare != IBHA_CSVD_DATE_EXACT) {
        /* Spec 5.3 makes value comparison of dates opt in *with an explicit input
         * format list*, precisely so that nothing guesses between 1/5/2024 and
         * 5/1/2024. The format list does not exist yet, so accepting the option
         * and silently comparing exactly would be worse than refusing it. */
        ibha_err(ctx, IBHA_CSVD_ERR_UNIMPLEMENTED,
                 "parse: date value comparison needs an input format list, which is not "
                 "implemented; use IBHA_CSVD_DATE_EXACT");
        return NULL;
    }

    if (p->opts.dialect.delimiter == p->opts.dialect.quote) {
        ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG, "parse: delimiter and quote cannot be the same byte");
        return NULL;
    }
    if (p->opts.header.rows == IBHA_CSVD_HEADER_AUTO &&
        (!p->opts.expect_table || !p->opts.expect_schema)) {
        ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG,
                 "parse: header auto-detection needs the source table and schema");
        return NULL;
    }
    if (p->opts.header.rows != IBHA_CSVD_HEADER_AUTO && p->opts.header.rows > 64) {
        ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG, "parse: header.rows of %u is not plausible",
                 p->opts.header.rows);
        return NULL;
    }

    build_tables(p);
    p->tbl.quote = p->opts.dialect.quote;
    p->header_rows = p->opts.header.rows;
    p->state = S_FIELD_START;
    p->bom_pending = p->opts.dialect.strip_bom ? 1 : 0;
    p->schema.key_row = IBHA_CSVD_NO_ROW;
    p->schema.required_row = IBHA_CSVD_NO_ROW;
    p->schema.type_row = IBHA_CSVD_NO_ROW;
    p->schema.name_row = IBHA_CSVD_NO_ROW;

    /* One row of index up front so an empty file still has its sentinel. */
    if (!rows_reserve(p, 2, 0)) {
        ibha_err(ctx, IBHA_CSVD_ERR_OOM, "parse: cannot allocate the row index");
        return NULL;
    }
    p->tbl.row_first_field[0] = 0;
    return p;
}

ibha_csvd_status ibha_csvd_parse_chunk(ibha_csvd_parser *p, const void *bytes, size_t len) {
    if (!p) return IBHA_CSVD_ERR_INVALID_ARG;
    if (p->ctx->status != IBHA_CSVD_OK) return p->ctx->status;
    if (p->finished) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_INVALID_ARG, "parse: chunk pushed after finish");
    }
    if (p->borrowed) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_INVALID_ARG,
                        "parse: cannot mix borrowed bytes with pushed chunks");
    }
    if (len == 0) return IBHA_CSVD_OK;

    uint64_t total = (uint64_t)p->buf.len + (uint64_t)len;
    if (total > p->ctx->limits.max_bytes) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_TOO_LARGE,
                        "File too large. Maximum allowed is %llu MB, received at least %llu MB.",
                        (uint64_t)(p->ctx->limits.max_bytes / (1024 * 1024)),
                        (uint64_t)((total + 1024 * 1024 - 1) / (1024 * 1024)));
    }
    /* Field offsets are u32, per spec 3.2. The default limit is far below this,
     * but a caller may raise it and the index must not silently wrap. */
    if (total > 0xFFFFFFF0ull) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_TOO_LARGE,
                        "File too large. The columnar index addresses at most 4 GB.");
    }

    /* Sized once, here rather than at begin, so a parser that turns out to
     * borrow its bytes never allocates a buffer it will not use. */
    if (p->buf.cap == 0 && p->opts.size_hint > 0 &&
        p->opts.size_hint <= p->ctx->limits.max_bytes) {
        if (!ibha_bytebuf_reserve(&p->ctx->arena, &p->buf, (size_t)p->opts.size_hint)) {
            return ibha_err(p->ctx, IBHA_CSVD_ERR_OOM, "parse: cannot reserve %llu bytes",
                            p->opts.size_hint);
        }
    }

    if (!ibha_bytebuf_append(&p->ctx->arena, &p->buf, (const uint8_t *)bytes, len)) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_OOM, "parse: cannot retain %llu bytes", total);
    }
    p->tbl.bytes = p->buf.bytes;
    p->tbl.len = p->buf.len;
    p->stats.bytes = p->buf.len;
    return run(p);
}

ibha_csvd_status ibha_csvd_parse_borrow(ibha_csvd_parser *p, const void *bytes, size_t len) {
    if (!p) return IBHA_CSVD_ERR_INVALID_ARG;
    if (p->ctx->status != IBHA_CSVD_OK) return p->ctx->status;
    if (p->finished || p->buf.len || p->borrowed) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_INVALID_ARG,
                        "parse: borrow must be the only input to a parser");
    }
    if (len > p->ctx->limits.max_bytes) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_TOO_LARGE,
                        "File too large. Maximum allowed is %llu MB, received at least %llu MB.",
                        (uint64_t)(p->ctx->limits.max_bytes / (1024 * 1024)),
                        (uint64_t)(((uint64_t)len + 1024 * 1024 - 1) / (1024 * 1024)));
    }
    if ((uint64_t)len > 0xFFFFFFF0ull) {
        return ibha_err(p->ctx, IBHA_CSVD_ERR_TOO_LARGE,
                        "File too large. The columnar index addresses at most 4 GB.");
    }

    p->borrowed = 1;
    p->tbl.bytes = (const uint8_t *)bytes;
    p->tbl.len = len;
    p->stats.bytes = len;
    return run(p);
}

/* Finishes the record left open by the end of the stream. Every state has a
 * defined meaning at EOF and only one of them is an error. */
static ibha_csvd_status finish_record(ibha_csvd_parser *p) {
    uint32_t at = (uint32_t)p->tbl.len;

    switch (p->state) {
        case S_QUOTED:
        case S_QUOTE_IN_QUOTED:
            if (p->state == S_QUOTED) {
                return ibha_err(p->ctx, IBHA_CSVD_ERR_UNTERMINATED_QUOTE,
                                "unterminated quoted field opened at row %u, field %u",
                                p->quote_row, p->quote_col);
            }
            /* The quote that closed the field was the last byte of the file. */
            {
                ibha_csvd_status st =
                    field_push(p, p->field_start, at - 1 - p->field_start, p->field_flags);
                if (st != IBHA_CSVD_OK) return st;
            }
            return row_end(p);

        case S_AFTER_QUOTED: return row_end(p);

        case S_UNQUOTED: {
            ibha_csvd_status st = field_push(p, p->field_start, at - p->field_start, p->field_flags);
            if (st != IBHA_CSVD_OK) return st;
            return row_end(p);
        }

        case S_FIELD_START:
            /* Mid record, as in "a,b," at EOF: the trailing empty field is real.
             * At a record boundary there is nothing open and a trailing newline
             * must not invent a row. */
            if (p->tbl.n_fields > p->row_first) {
                ibha_csvd_status st = field_push(p, p->field_start, 0, p->field_flags);
                if (st != IBHA_CSVD_OK) return st;
                return row_end(p);
            }
            return IBHA_CSVD_OK;

        case S_AFTER_CR:
        default: return IBHA_CSVD_OK; /* the record already ended */
    }
}

ibha_csvd_status ibha_csvd_parse_finish(ibha_csvd_parser *p) {
    if (!p) return IBHA_CSVD_ERR_INVALID_ARG;
    if (p->ctx->status != IBHA_CSVD_OK) return p->ctx->status;
    if (p->finished) return IBHA_CSVD_OK;
    p->finished = 1;

    /* A one or two byte file that turned out not to start with a BOM. */
    if (p->bom_pending) {
        p->bom_pending = 0;
        ibha_csvd_status st = run(p);
        if (st != IBHA_CSVD_OK) return st;
    }

    ibha_csvd_status st = finish_record(p);
    if (st != IBHA_CSVD_OK) return st;

    if (!p->schema_resolved) {
        st = ibha_schema_resolve(p->ctx, &p->tbl, &p->opts.header, p->opts.expect_table,
                                 p->opts.expect_schema, p->header_rows, &p->opts.compare,
                                 &p->schema);
        if (st != IBHA_CSVD_OK) return st;
        p->schema_resolved = 1;

        /* Only reachable when the file ended inside the header block, so there
         * are no data rows to digest. */
    }

    p->tbl.row_first_field[p->tbl.n_rows] = p->tbl.n_fields;
    p->stats.n_rows = p->tbl.n_rows;
    p->stats.n_fields = p->tbl.n_fields;
    p->stats.n_columns = p->tbl.n_columns;
    /* Stamped once the schema is settled, because it covers the key set and the
     * declared types as well as the comparison settings. The diff refuses a pair
     * whose stamps disagree rather than comparing digests that mean different
     * things. */
    p->tbl.compare_id = ibha_compare_id(&p->opts.compare, &p->schema);
    p->tbl.has_digests = p->opts.hash_rows ? 1u : 0u;
    return IBHA_CSVD_OK;
}

const ibha_csvd_table *ibha_csvd_table_of(const ibha_csvd_parser *p) { return p ? &p->tbl : NULL; }

const ibha_csvd_schema *ibha_csvd_schema_of(const ibha_csvd_parser *p) {
    return p ? &p->schema : NULL;
}

const ibha_csvd_parse_stats *ibha_csvd_parse_stats_of(const ibha_csvd_parser *p) {
    return p ? &p->stats : NULL;
}

/* 256 KB, matching ingest: large enough to amortize the reader call, small
 * enough that the freshly copied bytes are still in cache when parsed. */
#define PARSE_CHUNK (256u * 1024u)

ibha_csvd_status ibha_csvd_parse_stream(ibha_csvd_ctx *ctx, ibha_csvd_read_fn read, void *read_ctx,
                                        const ibha_csvd_parse_opts *opts, ibha_csvd_parser **out) {
    if (!ctx) return IBHA_CSVD_ERR_INVALID_ARG;
    if (!read) return ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG, "parse: null read callback");

    ibha_csvd_parser *p = ibha_csvd_parse_begin(ctx, opts);
    if (!p) return ctx->status;
    if (out) *out = p;

    uint8_t *chunk = (uint8_t *)ibha_arena_alloc(&ctx->arena, PARSE_CHUNK);
    if (!chunk) return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "parse: cannot allocate the read buffer");

    for (;;) {
        int64_t n = read(read_ctx, chunk, PARSE_CHUNK);
        if (n < 0) return ibha_err(ctx, IBHA_CSVD_ERR_IO, "parse: source read failed");
        if (n == 0) break;
        ibha_csvd_status st = ibha_csvd_parse_chunk(p, chunk, (size_t)n);
        if (st != IBHA_CSVD_OK) return st;
    }
    return ibha_csvd_parse_finish(p);
}

/* ---------------------------------------------------------- field access -- */

uint32_t ibha_csvd_row_field(const ibha_csvd_table *t, uint32_t row, uint32_t col) {
    if (!t || row >= t->n_rows) return 0xFFFFFFFFu;
    uint32_t first = t->row_first_field[row];
    if (col >= t->row_first_field[row + 1] - first) return 0xFFFFFFFFu;
    return first + col;
}

uint32_t ibha_csvd_row_field_count(const ibha_csvd_table *t, uint32_t row) {
    if (!t || row >= t->n_rows) return 0;
    return t->row_first_field[row + 1] - t->row_first_field[row];
}

int ibha_csvd_field_cmp(const ibha_csvd_table *ta, uint32_t fa, const ibha_csvd_table *tb,
                        uint32_t fb) {
    if (!ta || !tb || fa >= ta->n_fields || fb >= tb->n_fields) return 0;
    return ibha_field_cmp_raw(ta->bytes + ta->field_off[fa], ta->field_len[fa],
                              ta->field_flags[fa] & IBHA_CSVD_FIELD_HAS_ESCAPE,
                              tb->bytes + tb->field_off[fb], tb->field_len[fb],
                              tb->field_flags[fb] & IBHA_CSVD_FIELD_HAS_ESCAPE, ta->quote);
}

int ibha_csvd_field_cmp_str(const ibha_csvd_table *t, uint32_t f, const char *s) {
    if (!t || f >= t->n_fields || !s) return 0;
    size_t n = IBHA_STRLEN(s);
    return ibha_field_cmp_raw(t->bytes + t->field_off[f], t->field_len[f],
                              t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE, (const uint8_t *)s,
                              (uint32_t)n, 0, t->quote);
}

uint64_t ibha_csvd_field_hash(const ibha_csvd_table *t, uint32_t f) {
    if (!t || f >= t->n_fields) return 0;
    return ibha_field_hash_raw(t->bytes + t->field_off[f], t->field_len[f],
                               t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE, t->quote);
}

uint32_t ibha_csvd_field_logical_len(const ibha_csvd_table *t, uint32_t f) {
    if (!t || f >= t->n_fields) return 0;
    return ibha_field_logical_len_raw(t->bytes + t->field_off[f], t->field_len[f],
                                      t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE, t->quote);
}

uint32_t ibha_csvd_field_copy(const ibha_csvd_table *t, uint32_t f, uint8_t *dst, size_t cap) {
    if (!t || f >= t->n_fields) return 0;
    return ibha_field_copy_raw(t->bytes + t->field_off[f], t->field_len[f],
                               t->field_flags[f] & IBHA_CSVD_FIELD_HAS_ESCAPE, t->quote, dst, cap);
}
