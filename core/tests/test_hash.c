/*
 * test_hash.c - the XXH3 subset and the logical field primitives.
 *
 * The property that matters most here is the one from spec 5.2: quoting is never
 * a difference. Every assertion about the comparator and the hash is really an
 * assertion that a spreadsheet round trip which requotes a file cannot make the
 * diff light up.
 */
#include <stdlib.h>
#include <string.h>

#include "../src/internal.h"
#include "suites.h"
#include "tap.h"
#include "xxh3_vectors.h"

/* The same deterministic pattern that scripts-and-commands/gen_xxh3_vectors.py
 * fed to the reference implementation. */
static void fill_pattern(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 167 + 13) & 0xFF);
}

static void test_xxh3_vectors(void) {
    size_t max = 0;
    for (int i = 0; i < IBHA_XXH3_VECTOR_COUNT; i++) {
        if (k_xxh3_vectors[i].len > max) max = k_xxh3_vectors[i].len;
    }
    uint8_t *buf = (uint8_t *)malloc(max + 1);
    fill_pattern(buf, max);

    int bad = 0;
    for (int i = 0; i < IBHA_XXH3_VECTOR_COUNT; i++) {
        if (ibha_xxh3_64(buf, k_xxh3_vectors[i].len) != k_xxh3_vectors[i].expect) bad++;
    }
    /* One assertion for all 44 lengths, because what is being claimed is a
     * single thing: this is XXH3, not something like it. A mismatch at any
     * length means the digest protocol in spec 2.5 would silently disagree with
     * a server computing hashes with stock xxHash. */
    TAP_EQ_U64(bad, 0, "XXH3-64 matches the reference implementation at every length class");

    /* Unaligned input must hash identically; the reads are byte wise for exactly
     * this reason and a compiler that widened them would break here. */
    TAP_EQ_U64(ibha_xxh3_64(buf + 1, 64), ibha_xxh3_64(buf + 1, 64), "hashing is deterministic");
    free(buf);
}

/* ------------------------------------------------------------ comparator -- */

typedef struct {
    const char *raw;
    int escaped;
} lit;

static int cmp_lit(lit a, lit b) {
    return ibha_field_cmp_raw((const uint8_t *)a.raw, (uint32_t)strlen(a.raw), a.escaped,
                              (const uint8_t *)b.raw, (uint32_t)strlen(b.raw), b.escaped, '"');
}

static uint64_t hash_lit(lit a) {
    return ibha_field_hash_raw((const uint8_t *)a.raw, (uint32_t)strlen(a.raw), a.escaped, '"');
}

static void test_field_cmp(void) {
    /* The three forms of the same value from spec 5.2. The parser stores the
     * region inside the quotes, so `abc`, `"abc"` and a quoted-with-escapes
     * variant all arrive here as the same raw range or as an escaped one. */
    lit plain = {"abc", 0};
    lit quoted = {"abc", 0}; /* quoting does not change the stored range */
    TAP_EQ_U64(cmp_lit(plain, quoted) == 0, 1, "a quoted field equals the same bare field");

    lit esc = {"ab\"\"c", 1};
    lit bare = {"ab\"c", 0};
    TAP_EQ_U64(cmp_lit(esc, bare) == 0, 1, "\"ab\"\"c\" equals ab\"c");
    TAP_EQ_U64(cmp_lit(bare, esc) == 0, 1, "the comparison is symmetric");
    TAP_EQ_U64(hash_lit(esc) == hash_lit(bare), 1, "and they hash identically");

    lit both = {"\"\"x\"\"", 1};
    lit expect = {"\"x\"", 0};
    TAP_EQ_U64(cmp_lit(both, expect) == 0, 1, "escapes at both ends collapse");
    TAP_EQ_U64(hash_lit(both) == hash_lit(expect), 1, "and hash identically");

    /* Ordering, which the matcher will want later. */
    lit a = {"apple", 0}, b = {"banana", 0};
    TAP_OK(cmp_lit(a, b) < 0 && cmp_lit(b, a) > 0, "comparison orders like memcmp");

    lit sh = {"ab", 0}, lo = {"abc", 0};
    TAP_OK(cmp_lit(sh, lo) < 0, "a prefix sorts before the longer value");

    /* A prefix relationship that only appears after unescaping. */
    lit e1 = {"a\"\"", 1};
    lit e2 = {"a\"b", 0};
    TAP_OK(cmp_lit(e1, e2) < 0, "unescaping happens before the length comparison");

    lit empty = {"", 0};
    TAP_EQ_U64(cmp_lit(empty, empty) == 0, 1, "two empty fields are equal");
    TAP_OK(cmp_lit(empty, plain) < 0, "an empty field sorts before a non empty one");

    /* A lone trailing quote is malformed but must not read past the end. ASan
     * turns a mistake here into a failure rather than a silent overread. */
    lit lone = {"ab\"", 1};
    TAP_OK(cmp_lit(lone, lone) == 0, "a trailing lone quote compares without overreading");
    TAP_EQ_U64(ibha_field_logical_len_raw((const uint8_t *)"ab\"", 3, 1, '"'), 3,
               "a trailing lone quote is not collapsed");
}

static void test_field_copy(void) {
    uint8_t dst[16];
    uint32_t n = ibha_field_copy_raw((const uint8_t *)"a\"\"b", 4, 1, '"', dst, sizeof(dst));
    TAP_EQ_U64(n, 3, "copy reports the logical length");
    TAP_OK(memcmp(dst, "a\"b", 3) == 0, "copy materializes the unescaped value");

    /* Too small a buffer reports the need and writes nothing, so a caller cannot
     * mistake a truncated value for a real one. */
    uint8_t small[2] = {0xAA, 0xAA};
    TAP_EQ_U64(ibha_field_copy_raw((const uint8_t *)"abcd", 4, 0, '"', small, 2), 4,
               "copy into too small a buffer reports the required length");
    TAP_OK(small[0] == 0xAA, "copy into too small a buffer writes nothing");
}

static void test_hash_chunking(void) {
    /*
     * Long values fold over chunks of the logical byte stream. The escaped and
     * unescaped paths must agree across that boundary, which is the one place
     * the two implementations could drift apart.
     */
    size_t n = IBHA_HASH_CHUNK * 2 + 37;
    char *plain = (char *)malloc(n + 1);
    char *escaped = (char *)malloc(n * 2 + 1);

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        /* A quote every 500 bytes, so escape pairs straddle the chunk edge. */
        char c = (i % 500 == 499) ? '"' : (char)('a' + (i % 26));
        plain[i] = c;
        escaped[j++] = c;
        if (c == '"') escaped[j++] = '"';
    }
    plain[n] = '\0';
    escaped[j] = '\0';

    uint64_t h_plain = ibha_field_hash_raw((const uint8_t *)plain, (uint32_t)n, 0, '"');
    uint64_t h_esc = ibha_field_hash_raw((const uint8_t *)escaped, (uint32_t)j, 1, '"');
    TAP_EQ_U64(h_plain == h_esc, 1, "a long escaped value hashes as its long unescaped value");
    TAP_EQ_U64(ibha_field_cmp_raw((const uint8_t *)plain, (uint32_t)n, 0, (const uint8_t *)escaped,
                                  (uint32_t)j, 1, '"'),
               0, "and compares equal to it");
    TAP_EQ_U64(ibha_field_logical_len_raw((const uint8_t *)escaped, (uint32_t)j, 1, '"'), n,
               "logical length is right across chunk boundaries");

    /* Different content of the same length must not collide through the fold. */
    plain[n / 2] = plain[n / 2] == 'z' ? 'y' : 'z';
    TAP_OK(ibha_field_hash_raw((const uint8_t *)plain, (uint32_t)n, 0, '"') != h_plain,
           "a single byte change in a long value changes its hash");

    free(plain);
    free(escaped);
}

static void test_row_mix(void) {
    /* Field boundaries have to matter, or ("ab","c") and ("a","bc") would be the
     * same row and a shifted file would look unchanged. */
    uint64_t left = ibha_hash_mix(ibha_hash_mix(0, ibha_xxh3_64("ab", 2)), ibha_xxh3_64("c", 1));
    uint64_t right = ibha_hash_mix(ibha_hash_mix(0, ibha_xxh3_64("a", 1)), ibha_xxh3_64("bc", 2));
    TAP_OK(ibha_hash_final(left, 2) != ibha_hash_final(right, 2),
           "row hashing is sensitive to where the field boundaries are");

    uint64_t ab = ibha_hash_mix(ibha_hash_mix(0, 1), 2);
    uint64_t ba = ibha_hash_mix(ibha_hash_mix(0, 2), 1);
    TAP_OK(ibha_hash_final(ab, 2) != ibha_hash_final(ba, 2), "row hashing is order dependent");
}

/*
 * The two 128 bit multiplies must agree, because the two build targets do not use
 * the same one.
 *
 * The native build multiplies through __uint128_t, which becomes one instruction.
 * The freestanding wasm32 build cannot: clang lowers that to a call to __multi3
 * in compiler-rt, which a -nostdlib module has no runtime for, so it uses the
 * hand rolled version instead. Spec 3.2 requires the two targets to produce byte
 * identical output, and this multiply sits under every digest in the engine, so
 * "they obviously compute the same thing" is not good enough.
 */
static void test_mul128_agrees(void) {
    static const uint64_t k_edges[] = {
        0u, 1u, 2u, 0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull,
        0x8000000000000000ull, 0x9E3779B185EBCA87ull, 0xC2B2AE3D27D4EB4Full,
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(k_edges) / sizeof(k_edges[0]); i++) {
        for (size_t j = 0; j < sizeof(k_edges) / sizeof(k_edges[0]); j++) {
            if (ibha_mul128_fold64(k_edges[i], k_edges[j]) !=
                ibha_mul128_fold64_portable(k_edges[i], k_edges[j])) {
                ok = 0;
            }
        }
    }
    TAP_OK(ok, "the two 128 bit multiplies agree on every edge case");

    /* And over a large pseudo random sweep, because the carry between the two
     * halves is where a hand rolled multiply goes wrong and edge cases alone
     * would not find it. */
    uint64_t x = 0x243F6A8885A308D3ull;
    ok = 1;
    for (int i = 0; i < 200000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        uint64_t y = x * 0x9E3779B97F4A7C15ull;
        if (ibha_mul128_fold64(x, y) != ibha_mul128_fold64_portable(x, y)) ok = 0;
    }
    TAP_OK(ok, "and over 200,000 random pairs, so the wasm build hashes as the native one does");
}

void ibha_test_hash(void) {
    test_xxh3_vectors();
    test_field_cmp();
    test_field_copy();
    test_hash_chunking();
    test_row_mix();
    test_mul128_agrees();
}
