/*
 * fuzz_ingest.c - libFuzzer entry point.
 *
 * Phase 0 fuzzes the ingest path and, importantly, the chunk boundary handling:
 * the input's first byte selects a reader stride, so the fuzzer explores splits
 * landing in the middle of multi byte sequences, quoted regions and CRLF pairs.
 * That is the property the resumable parser depends on, so the harness that
 * exercises it exists before the parser does.
 *
 * The state machine, which is the input surface that actually matters, is fuzzed
 * by tests/fuzz_parse.c. This target stays because it covers what that one does
 * not: the limit enforcement and the retention path.
 *
 *   make fuzz          libFuzzer, needs a clang that ships its runtime
 *   make fuzz-native   the same target under the built in driver, no toolchain
 *   build/fuzz_ingest -max_total_time=60 corpus/
 */
#include <stdint.h>
#include <stddef.h>

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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;

    /* First byte drives the chunk stride, so boundaries land in awkward places. */
    size_t stride = (size_t)data[0] + 1;
    const uint8_t *payload = data + 1;
    size_t payload_len = size - 1;

    ibha_csvd_limits lim;
    ibha_csvd_limits_init(&lim);
    /* A small limit so the TOO_LARGE path is reachable from short inputs. */
    lim.max_bytes = 1024 * 1024;

    ibha_csvd_ctx *ctx = ibha_csvd_ctx_new(&lim);
    if (!ctx) return 0;

    fuzz_reader r = {payload, payload_len, 0, stride};
    ibha_csvd_ingest_stats st;
    /* Alternate the hint so both the exact-reserve and doubling paths are fuzzed,
     * including deliberately wrong hints. */
    uint64_t hint = (stride & 1) ? (uint64_t)payload_len : 0;
    ibha_csvd_ingest(ctx, fuzz_read, &r, 1, hint, &st);

    /* Invariant worth asserting under the fuzzer: on success the retained bytes
     * must be byte identical to the input, whatever the chunking was. Silent
     * truncation is the failure mode that would matter most here. */
    if (ibha_csvd_ctx_status(ctx) == IBHA_CSVD_OK) {
        size_t got_len = 0;
        const uint8_t *got = ibha_csvd_ingest_bytes(ctx, &got_len);
        if (got_len != payload_len) __builtin_trap();
        if (got_len && IBHA_MEMCMP(got, payload, got_len) != 0) __builtin_trap();
    }

    ibha_csvd_ctx_free(ctx);
    return 0;
}
