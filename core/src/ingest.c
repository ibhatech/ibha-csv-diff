/*
 * ingest.c - Phase 0 ingest path.
 *
 * Pulls a source to exhaustion through the reader callback, enforcing the byte
 * limit as bytes arrive, and tallies a few byte level counts.
 *
 * This is explicitly not the parser. It exists to establish the Phase 0
 * throughput baseline that the SIMD decision is gated on (spec section 11), and
 * to prove out the ingest path, the limit enforcement and the arena under real
 * sized inputs before any parsing logic lands on top.
 *
 * The scan loop here is the same shape the scalar parser will have: one pass,
 * one byte at a time, no allocation. So its throughput is a fair upper bound for
 * the parser, and the gap between the two later tells us what the state machine
 * itself costs.
 */
#include "internal.h"

/* 256 KB. Large enough to amortize the reader call, small enough to stay inside
 * L2 so the scan measures scan cost rather than memory bandwidth to a cold
 * buffer. */
#define INGEST_CHUNK (256u * 1024u)

const uint8_t *ibha_csvd_ingest_bytes(const ibha_csvd_ctx *ctx, size_t *len) {
    if (!ctx || !ctx->has_ingested) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = ctx->ingested.len;
    return ctx->ingested.bytes;
}

ibha_csvd_status ibha_csvd_ingest(ibha_csvd_ctx *ctx, ibha_csvd_read_fn read, void *read_ctx,
                                 int retain, uint64_t size_hint, ibha_csvd_ingest_stats *out) {
    if (!ctx) return IBHA_CSVD_ERR_INVALID_ARG;
    if (!read) return ibha_err(ctx, IBHA_CSVD_ERR_INVALID_ARG, "ingest: null read callback");
    if (ctx->status != IBHA_CSVD_OK) return ctx->status;

    /* A hint that already exceeds the limit is rejected before a single byte is
     * read. There is no point streaming 2 GB to discover it was too large when
     * the source told us up front. */
    if (size_hint > ctx->limits.max_bytes) {
        return ibha_err(ctx, IBHA_CSVD_ERR_TOO_LARGE,
                        "File too large. Maximum allowed is %llu MB, received %llu MB.",
                        (uint64_t)(ctx->limits.max_bytes / (1024 * 1024)),
                        (uint64_t)((size_hint + 1024 * 1024 - 1) / (1024 * 1024)));
    }

    uint8_t *chunk = (uint8_t *)ibha_arena_alloc(&ctx->arena, INGEST_CHUNK);
    if (!chunk) return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "ingest: cannot allocate read buffer");

    /* Size up the retain buffer once. Exact when the hint is right, and the
     * doubling path still covers the case where the source produces more than it
     * promised, so a wrong hint costs performance but never correctness. */
    if (retain && size_hint > 0) {
        if (!ibha_bytebuf_reserve(&ctx->arena, &ctx->ingested, (size_t)size_hint)) {
            return ibha_err(ctx, IBHA_CSVD_ERR_OOM, "ingest: cannot reserve %llu bytes", size_hint);
        }
    }

    ibha_csvd_ingest_stats st = {0, 0, 0};

    for (;;) {
        int64_t n = read(read_ctx, chunk, INGEST_CHUNK);
        if (n < 0) return ibha_err(ctx, IBHA_CSVD_ERR_IO, "ingest: source read failed");
        if (n == 0) break;

        st.bytes += (uint64_t)n;

        /* Limit checked here, on arrival, so an oversized stream fails at the
         * byte that crosses the threshold instead of after we have buffered it
         * all to discover the size. Per spec 13.4. */
        if (st.bytes > ctx->limits.max_bytes) {
            return ibha_err(ctx, IBHA_CSVD_ERR_TOO_LARGE,
                            "File too large. Maximum allowed is %llu MB, received at least %llu MB.",
                            (uint64_t)(ctx->limits.max_bytes / (1024 * 1024)),
                            (uint64_t)((st.bytes + 1024 * 1024 - 1) / (1024 * 1024)));
        }

        for (int64_t i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            st.line_breaks += (c == '\n');
            st.quotes += (c == '"');
        }

        if (retain) {
            if (!ibha_bytebuf_append(&ctx->arena, &ctx->ingested, chunk, (size_t)n)) {
                return ibha_err(ctx, IBHA_CSVD_ERR_OOM,
                                "ingest: cannot retain %llu bytes", st.bytes);
            }
            ctx->has_ingested = 1;
        }
    }

    if (out) *out = st;
    return IBHA_CSVD_OK;
}
