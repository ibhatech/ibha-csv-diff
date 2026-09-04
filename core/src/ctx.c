#include <stdarg.h> /* freestanding header, available under -nostdlib */

#include "internal.h"

void ibha_csvd_version(int *major, int *minor, int *patch) {
    if (major) *major = IBHA_CSVD_VERSION_MAJOR;
    if (minor) *minor = IBHA_CSVD_VERSION_MINOR;
    if (patch) *patch = IBHA_CSVD_VERSION_PATCH;
}

const char *ibha_csvd_status_name(ibha_csvd_status st) {
    switch (st) {
        case IBHA_CSVD_OK: return "OK";
        case IBHA_CSVD_ERR_OOM: return "OUT_OF_MEMORY";
        case IBHA_CSVD_ERR_IO: return "IO_ERROR";
        case IBHA_CSVD_ERR_TOO_LARGE: return "TOO_LARGE";
        case IBHA_CSVD_ERR_UNTERMINATED_QUOTE: return "UNTERMINATED_QUOTE";
        case IBHA_CSVD_ERR_RAGGED_ROW: return "RAGGED_ROW";
        case IBHA_CSVD_ERR_DUPLICATE_KEY: return "DUPLICATE_KEY";
        case IBHA_CSVD_ERR_COLUMN_ORDER: return "COLUMN_ORDER";
        case IBHA_CSVD_ERR_NO_HEADER: return "NO_HEADER";
        case IBHA_CSVD_ERR_MISSING_KEY_COLUMN: return "MISSING_KEY_COLUMN";
        case IBHA_CSVD_ERR_BAD_CONTENT: return "BAD_CONTENT";
        case IBHA_CSVD_ERR_INVALID_ARG: return "INVALID_ARG";
        case IBHA_CSVD_ERR_UNIMPLEMENTED: return "UNIMPLEMENTED";
    }
    return "UNKNOWN";
}

void ibha_csvd_limits_init(ibha_csvd_limits *out) {
    if (!out) return;
    out->max_bytes = IBHA_CSVD_DEFAULT_MAX_BYTES;
    out->max_rows = 0;
    out->max_columns = 0;
}

/* ------------------------------------------------------- tiny formatter ---- */
/*
 * The freestanding build has no snprintf, and pulling one in would cost more
 * code size than the engine's own parser. This handles exactly the conversions
 * the error messages need: %s, %c, %d, %u, %llu, %zu, %%.
 * Always NUL terminates. Truncates rather than overflowing.
 */
typedef struct {
    char *out;
    size_t cap;
    size_t len;
} fmt_sink;

static void fmt_ch(fmt_sink *s, char c) {
    if (s->len + 1 < s->cap) s->out[s->len] = c;
    s->len++;
}

static void fmt_str(fmt_sink *s, const char *p) {
    if (!p) p = "(null)";
    while (*p) fmt_ch(s, *p++);
}

static void fmt_u64(fmt_sink *s, uint64_t v) {
    char tmp[20];
    int n = 0;
    if (v == 0) {
        fmt_ch(s, '0');
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) fmt_ch(s, tmp[--n]);
}

static void fmt_i64(fmt_sink *s, int64_t v) {
    if (v < 0) {
        fmt_ch(s, '-');
        /* Negating INT64_MIN is undefined, so widen through uint64_t. */
        fmt_u64(s, (uint64_t)0 - (uint64_t)v);
    } else {
        fmt_u64(s, (uint64_t)v);
    }
}

static void ibha_vfmt(char *out, size_t cap, const char *fmt, va_list ap) {
    fmt_sink s = {out, cap, 0};
    if (cap == 0) return;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            fmt_ch(&s, *p);
            continue;
        }
        p++;
        if (*p == 0) break;
        switch (*p) {
            case 's': fmt_str(&s, va_arg(ap, const char *)); break;
            case 'c': fmt_ch(&s, (char)va_arg(ap, int)); break;
            case 'd': fmt_i64(&s, (int64_t)va_arg(ap, int)); break;
            case 'u': fmt_u64(&s, (uint64_t)va_arg(ap, unsigned int)); break;
            case 'z':
                if (*(p + 1) == 'u') {
                    p++;
                    fmt_u64(&s, (uint64_t)va_arg(ap, size_t));
                }
                break;
            case 'l':
                if (*(p + 1) == 'l' && *(p + 2) == 'u') {
                    p += 2;
                    fmt_u64(&s, va_arg(ap, uint64_t));
                } else if (*(p + 1) == 'l' && *(p + 2) == 'd') {
                    p += 2;
                    fmt_i64(&s, va_arg(ap, int64_t));
                }
                break;
            case '%': fmt_ch(&s, '%'); break;
            default: fmt_ch(&s, '%'); fmt_ch(&s, *p); break;
        }
    }
    out[s.len < cap ? s.len : cap - 1] = '\0';
}

ibha_csvd_status ibha_err(ibha_csvd_ctx *ctx, ibha_csvd_status st, const char *fmt, ...) {
    if (!ctx) return st;

    /* First error wins, per spec 13.5. A later symptom must not overwrite the
     * original cause, otherwise the message the user sees points at the wrong
     * place. */
    if (ctx->status != IBHA_CSVD_OK) return ctx->status;

    ctx->status = st;
    va_list ap;
    va_start(ap, fmt);
    ibha_vfmt(ctx->errmsg, sizeof(ctx->errmsg), fmt, ap);
    va_end(ap);
    return st;
}

/* ------------------------------------------------------------- lifecycle ---- */

ibha_csvd_ctx *ibha_csvd_ctx_new(const ibha_csvd_limits *limits) {
    ibha_arena bootstrap;
    ibha_arena_init(&bootstrap);

    ibha_csvd_ctx *ctx = (ibha_csvd_ctx *)ibha_arena_calloc(&bootstrap, sizeof(*ctx));
    if (!ctx) {
        ibha_arena_destroy(&bootstrap);
        return NULL;
    }

    /* The context lives inside its own arena, so move the bootstrap arena's
     * bookkeeping into it. From here the context owns every byte. */
    ctx->arena = bootstrap;
    ctx->status = IBHA_CSVD_OK;
    ctx->errmsg[0] = '\0';

    if (limits) {
        ctx->limits = *limits;
        if (ctx->limits.max_bytes == 0) ctx->limits.max_bytes = IBHA_CSVD_DEFAULT_MAX_BYTES;
    } else {
        ibha_csvd_limits_init(&ctx->limits);
    }
    return ctx;
}

void ibha_csvd_ctx_free(ibha_csvd_ctx *ctx) {
    if (!ctx) return;
    /* The context struct is itself arena memory, so copy the arena out before
     * destroying it. Touching ctx after this point is a use after free. */
    ibha_arena arena = ctx->arena;
    ibha_arena_destroy(&arena);
}

ibha_csvd_status ibha_csvd_ctx_status(const ibha_csvd_ctx *ctx) {
    return ctx ? ctx->status : IBHA_CSVD_ERR_INVALID_ARG;
}

const char *ibha_csvd_ctx_error(const ibha_csvd_ctx *ctx) {
    if (!ctx) return "null context";
    return ctx->errmsg;
}

uint64_t ibha_csvd_ctx_bytes_reserved(const ibha_csvd_ctx *ctx) {
    return ctx ? ctx->arena.reserved : 0;
}
