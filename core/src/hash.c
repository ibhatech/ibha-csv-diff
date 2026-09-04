/*
 * hash.c - the XXH3 subset and the logical field primitives.
 *
 * Two things live here and they are related by one rule from spec 5.2: quoting
 * must never surface as a difference. Everything below therefore operates on the
 * *logical* value of a field, meaning the bytes inside the quotes with every ""
 * pair collapsed to a single ", and none of it allocates or builds a string.
 *
 * The hash is XXH3, 64 bit, one shot, unseeded, with the default secret. That is
 * the only subset the engine needs: streaming XXH3 would buy nothing here
 * because a field is always contiguous in memory. It is verified bit for bit
 * against the reference implementation in tests/test_hash.c, which matters
 * because the digest protocol in spec 2.5 would have a server computing these
 * with stock xxHash, and a hash that is "close enough" would silently produce
 * wrong diffs rather than failing.
 */
#include "internal.h"

#define PRIME32_1 0x9E3779B1u
#define PRIME32_2 0x85EBCA77u
#define PRIME32_3 0xC2B2AE3Du
#define PRIME64_1 0x9E3779B185EBCA87ull
#define PRIME64_2 0xC2B2AE3D27D4EB4Full
#define PRIME64_3 0x165667B19E3779F9ull
#define PRIME64_4 0x85EBCA77C2B2AE63ull
#define PRIME64_5 0x27D4EB2F165667C5ull

#define XXH_SECRET_SIZE 192
/* The reference implementation indexes the 129..240 tail from its minimum
 * secret size, not from the length of the default secret. */
#define XXH_SECRET_SIZE_MIN 136
#define XXH_STRIPE_LEN 64
#define XXH_ACC_NB 8
#define XXH_SECRET_CONSUME_RATE 8
#define XXH_SECRET_LASTACC_START 7
#define XXH_SECRET_MERGEACCS_START 11

/* The default secret from the reference implementation. Changing a single byte
 * changes every hash this library produces. */
static const uint8_t k_secret[XXH_SECRET_SIZE] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

/* Little endian reads that do not assume alignment. The compiler folds these
 * into single instructions on every target we build for. */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint64_t rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static uint64_t swap64(uint64_t x) {
    return ((x & 0x00000000000000ffull) << 56) | ((x & 0x000000000000ff00ull) << 40) |
           ((x & 0x0000000000ff0000ull) << 24) | ((x & 0x00000000ff000000ull) << 8) |
           ((x & 0x000000ff00000000ull) >> 8) | ((x & 0x0000ff0000000000ull) >> 24) |
           ((x & 0x00ff000000000000ull) >> 40) | ((x & 0xff00000000000000ull) >> 56);
}

/*
 * 64x64 -> 128 multiply, folded to 64 bits, by hand out of 32 bit halves.
 * `lower` and `upper` are exactly the low and high words of the true product, so
 * this returns the same value the __uint128_t expression below does, on any
 * target, with no reliance on a compiler runtime.
 */
uint64_t ibha_mul128_fold64_portable(uint64_t a, uint64_t b) {
    uint64_t lo_lo = (uint64_t)(a & 0xffffffffu) * (uint64_t)(b & 0xffffffffu);
    uint64_t hi_lo = (uint64_t)(a >> 32) * (uint64_t)(b & 0xffffffffu);
    uint64_t lo_hi = (uint64_t)(a & 0xffffffffu) * (uint64_t)(b >> 32);
    uint64_t hi_hi = (uint64_t)(a >> 32) * (uint64_t)(b >> 32);
    uint64_t cross = (lo_lo >> 32) + (hi_lo & 0xffffffffu) + lo_hi;
    uint64_t upper = (hi_lo >> 32) + (cross >> 32) + hi_hi;
    uint64_t lower = (cross << 32) | (lo_lo & 0xffffffffu);
    return lower ^ upper;
}

/*
 * The one the engine uses.
 *
 * **wasm32 is excluded from the __uint128_t path deliberately, and not because
 * clang lacks the type.** It has it, and defines __SIZEOF_INT128__ to say so, but
 * it lowers the multiply to a call to __multi3 in compiler-rt. A freestanding
 * -nostdlib build has no compiler-rt, so the module linked with an undefined
 * import of `env.__multi3` and would have needed the JavaScript host to supply a
 * 128 bit multiply: slower than the code below, and one more place for the wasm
 * build to disagree with the native one about a hash value. Spec 3.2 requires
 * the two targets to produce byte identical output, and a digest is the last
 * place to take a chance on that.
 *
 * That the two paths agree is asserted directly in tests/test_hash.c rather than
 * argued for here.
 */
uint64_t ibha_mul128_fold64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__) && !defined(__wasm__)
    __uint128_t product = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)product ^ (uint64_t)(product >> 64);
#else
    return ibha_mul128_fold64_portable(a, b);
#endif
}

#define mul128_fold64 ibha_mul128_fold64

static uint64_t xxh64_avalanche(uint64_t h) {
    h ^= h >> 33;
    h *= PRIME64_2;
    h ^= h >> 29;
    h *= PRIME64_3;
    h ^= h >> 32;
    return h;
}

static uint64_t xxh3_avalanche(uint64_t h) {
    h ^= h >> 37;
    h *= 0x165667919E3779F9ull;
    h ^= h >> 32;
    return h;
}

static uint64_t xxh3_rrmxmx(uint64_t h, uint64_t len) {
    h ^= rotl64(h, 49) ^ rotl64(h, 24);
    h *= 0x9FB21C651E98DF25ull;
    h ^= (h >> 35) + len;
    h *= 0x9FB21C651E98DF25ull;
    h ^= h >> 28;
    return h;
}

static uint64_t xxh3_len_1to3(const uint8_t *p, size_t len) {
    uint8_t c1 = p[0];
    uint8_t c2 = p[len >> 1];
    uint8_t c3 = p[len - 1];
    uint32_t combined = ((uint32_t)c1 << 16) | ((uint32_t)c2 << 24) | ((uint32_t)c3 << 0) |
                        ((uint32_t)len << 8);
    uint64_t keyed = (uint64_t)combined ^ (uint64_t)(rd32(k_secret) ^ rd32(k_secret + 4));
    return xxh64_avalanche(keyed);
}

static uint64_t xxh3_len_4to8(const uint8_t *p, size_t len) {
    uint32_t lo = rd32(p);
    uint32_t hi = rd32(p + len - 4);
    uint64_t input = (uint64_t)hi | ((uint64_t)lo << 32);
    uint64_t secret = rd64(k_secret + 8) ^ rd64(k_secret + 16);
    uint64_t keyed = input ^ secret;
    return xxh3_rrmxmx(keyed, (uint64_t)len);
}

static uint64_t xxh3_len_9to16(const uint8_t *p, size_t len) {
    uint64_t lo_secret = rd64(k_secret + 24) ^ rd64(k_secret + 32);
    uint64_t hi_secret = rd64(k_secret + 40) ^ rd64(k_secret + 48);
    uint64_t lo = rd64(p) ^ lo_secret;
    uint64_t hi = rd64(p + len - 8) ^ hi_secret;
    uint64_t acc = (uint64_t)len + swap64(lo) + hi + mul128_fold64(lo, hi);
    return xxh3_avalanche(acc);
}

static uint64_t xxh3_mix16(const uint8_t *input, const uint8_t *secret) {
    uint64_t lo = rd64(input);
    uint64_t hi = rd64(input + 8);
    return mul128_fold64(lo ^ rd64(secret), hi ^ rd64(secret + 8));
}

static uint64_t xxh3_len_17to128(const uint8_t *p, size_t len) {
    uint64_t acc = (uint64_t)len * PRIME64_1;
    if (len > 32) {
        if (len > 64) {
            if (len > 96) {
                acc += xxh3_mix16(p + 48, k_secret + 96);
                acc += xxh3_mix16(p + len - 64, k_secret + 112);
            }
            acc += xxh3_mix16(p + 32, k_secret + 64);
            acc += xxh3_mix16(p + len - 48, k_secret + 80);
        }
        acc += xxh3_mix16(p + 16, k_secret + 32);
        acc += xxh3_mix16(p + len - 32, k_secret + 48);
    }
    acc += xxh3_mix16(p, k_secret);
    acc += xxh3_mix16(p + len - 16, k_secret + 16);
    return xxh3_avalanche(acc);
}

#define XXH3_MIDSIZE_STARTOFFSET 3
#define XXH3_MIDSIZE_LASTOFFSET 17

static uint64_t xxh3_len_129to240(const uint8_t *p, size_t len) {
    uint64_t acc = (uint64_t)len * PRIME64_1;
    size_t nb_rounds = len / 16;

    for (size_t i = 0; i < 8; i++) acc += xxh3_mix16(p + 16 * i, k_secret + 16 * i);
    acc = xxh3_avalanche(acc);

    for (size_t i = 8; i < nb_rounds; i++) {
        acc += xxh3_mix16(p + 16 * i, k_secret + 16 * (i - 8) + XXH3_MIDSIZE_STARTOFFSET);
    }
    acc += xxh3_mix16(p + len - 16, k_secret + XXH_SECRET_SIZE_MIN - XXH3_MIDSIZE_LASTOFFSET);
    return xxh3_avalanche(acc);
}

static void xxh3_accumulate_512(uint64_t *acc, const uint8_t *input, const uint8_t *secret) {
    for (size_t i = 0; i < XXH_ACC_NB; i++) {
        uint64_t data_val = rd64(input + 8 * i);
        uint64_t data_key = data_val ^ rd64(secret + 8 * i);
        acc[i ^ 1] += data_val; /* swap adjacent lanes, as the SIMD path does */
        acc[i] += (uint64_t)(uint32_t)data_key * (uint64_t)(data_key >> 32);
    }
}

static void xxh3_scramble_acc(uint64_t *acc, const uint8_t *secret) {
    for (size_t i = 0; i < XXH_ACC_NB; i++) {
        uint64_t a = acc[i];
        a ^= a >> 47;
        a ^= rd64(secret + 8 * i);
        a *= PRIME32_1;
        acc[i] = a;
    }
}

static uint64_t xxh3_mix_accs(const uint64_t *acc, const uint8_t *secret, uint64_t start) {
    uint64_t result = start;
    for (size_t i = 0; i < 4; i++) {
        result += mul128_fold64(acc[2 * i] ^ rd64(secret + 16 * i),
                                acc[2 * i + 1] ^ rd64(secret + 16 * i + 8));
    }
    return xxh3_avalanche(result);
}

static uint64_t xxh3_long(const uint8_t *p, size_t len) {
    uint64_t acc[XXH_ACC_NB] = {PRIME32_3, PRIME64_1, PRIME64_2, PRIME64_3,
                                PRIME64_4, PRIME32_2, PRIME64_5, PRIME32_1};

    size_t nb_stripes_per_block = (XXH_SECRET_SIZE - XXH_STRIPE_LEN) / XXH_SECRET_CONSUME_RATE;
    size_t block_len = XXH_STRIPE_LEN * nb_stripes_per_block;
    size_t nb_blocks = (len - 1) / block_len;

    for (size_t n = 0; n < nb_blocks; n++) {
        for (size_t i = 0; i < nb_stripes_per_block; i++) {
            xxh3_accumulate_512(acc, p + n * block_len + i * XXH_STRIPE_LEN,
                                k_secret + i * XXH_SECRET_CONSUME_RATE);
        }
        xxh3_scramble_acc(acc, k_secret + XXH_SECRET_SIZE - XXH_STRIPE_LEN);
    }

    size_t nb_stripes = ((len - 1) - (block_len * nb_blocks)) / XXH_STRIPE_LEN;
    for (size_t i = 0; i < nb_stripes; i++) {
        xxh3_accumulate_512(acc, p + nb_blocks * block_len + i * XXH_STRIPE_LEN,
                            k_secret + i * XXH_SECRET_CONSUME_RATE);
    }
    /* The last stripe always covers the final 64 bytes, overlapping the previous
     * one when len is not a multiple of the stripe length. */
    xxh3_accumulate_512(acc, p + len - XXH_STRIPE_LEN,
                        k_secret + XXH_SECRET_SIZE - XXH_STRIPE_LEN - XXH_SECRET_LASTACC_START);

    return xxh3_mix_accs(acc, k_secret + XXH_SECRET_MERGEACCS_START,
                         (uint64_t)len * PRIME64_1);
}

uint64_t ibha_xxh3_64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (len <= 16) {
        if (len > 8) return xxh3_len_9to16(p, len);
        if (len >= 4) return xxh3_len_4to8(p, len);
        if (len) return xxh3_len_1to3(p, len);
        return xxh64_avalanche(rd64(k_secret + 56) ^ rd64(k_secret + 64));
    }
    if (len <= 128) return xxh3_len_17to128(p, len);
    if (len <= 240) return xxh3_len_129to240(p, len);
    return xxh3_long(p, len);
}

/* ------------------------------------------------------------ combiners -- */

uint64_t ibha_hash_mix(uint64_t acc, uint64_t h) {
    /* Order dependent, so ("ab","c") and ("a","bc") cannot collide by
     * concatenation, which a plain XOR or addition would allow. */
    return rotl64(acc ^ h, 27) * PRIME64_1 + PRIME64_4;
}

uint64_t ibha_hash_final(uint64_t acc, uint64_t n) {
    return xxh3_avalanche(acc ^ (n * PRIME64_2));
}

/* --------------------------------------------------------------- fields -- */
/*
 * A field's logical value is its raw range with every "" collapsed to ". The
 * fast path, which is essentially every cell, has no escapes at all and is a
 * plain memcmp or a single hash call over the range as it sits.
 */

int ibha_field_cmp_raw(const uint8_t *a, uint32_t alen, int a_esc, const uint8_t *b, uint32_t blen,
                       int b_esc, uint8_t quote) {
    if (!a_esc && !b_esc) {
        uint32_t n = alen < blen ? alen : blen;
        int c = n ? IBHA_MEMCMP(a, b, n) : 0;
        if (c) return c;
        return alen < blen ? -1 : (alen > blen ? 1 : 0);
    }

    uint32_t i = 0, j = 0;
    while (i < alen && j < blen) {
        uint8_t ca = a[i];
        uint8_t cb = b[j];
        /* A "" pair contributes one quote and advances two. The bounds check
         * matters: a trailing lone quote is possible in a malformed field and
         * must not read past the end. */
        if (a_esc && ca == quote && i + 1 < alen && a[i + 1] == quote) i++;
        if (b_esc && cb == quote && j + 1 < blen && b[j + 1] == quote) j++;
        if (ca != cb) return ca < cb ? -1 : 1;
        i++;
        j++;
    }
    if (i < alen) return 1;
    if (j < blen) return -1;
    return 0;
}

uint32_t ibha_field_logical_len_raw(const uint8_t *p, uint32_t len, int esc, uint8_t quote) {
    if (!esc) return len;
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++, n++) {
        if (p[i] == quote && i + 1 < len && p[i + 1] == quote) i++;
    }
    return n;
}

uint32_t ibha_field_copy_raw(const uint8_t *p, uint32_t len, int esc, uint8_t quote, uint8_t *dst,
                             size_t cap) {
    uint32_t need = ibha_field_logical_len_raw(p, len, esc, quote);
    if (!dst || cap < (size_t)need) return need;

    if (!esc) {
        if (len) IBHA_MEMCPY(dst, p, len);
        return need;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] == quote && i + 1 < len && p[i + 1] == quote) i++;
        dst[n++] = p[i];
    }
    return n;
}

/*
 * Hashing a logical value without materializing it.
 *
 * Long values are folded in IBHA_HASH_CHUNK sized pieces of the *logical* byte
 * stream. That is what lets the escaped path collapse "" through a fixed stack
 * window and still land on the same value as the unescaped path: both sides
 * chunk at the same logical positions. Everything shorter than one chunk, which
 * is every realistic cell, is a single XXH3 call over the bytes in place.
 */
uint64_t ibha_field_hash_raw(const uint8_t *p, uint32_t len, int esc, uint8_t quote) {
    if (!esc) {
        if (len <= IBHA_HASH_CHUNK) return ibha_xxh3_64(p, len);
        uint64_t acc = 0;
        uint32_t off = 0;
        while (off < len) {
            uint32_t n = len - off < IBHA_HASH_CHUNK ? len - off : IBHA_HASH_CHUNK;
            acc = ibha_hash_mix(acc, ibha_xxh3_64(p + off, n));
            off += n;
        }
        return ibha_hash_final(acc, len);
    }

    uint8_t window[IBHA_HASH_CHUNK];
    uint64_t acc = 0;
    uint32_t filled = 0;
    uint32_t logical = 0;
    int folded = 0;

    for (uint32_t i = 0; i < len; i++) {
        if (p[i] == quote && i + 1 < len && p[i + 1] == quote) i++;
        window[filled++] = p[i];
        logical++;
        if (filled == IBHA_HASH_CHUNK) {
            acc = ibha_hash_mix(acc, ibha_xxh3_64(window, filled));
            filled = 0;
            folded = 1;
        }
    }

    if (!folded) return ibha_xxh3_64(window, filled);
    if (filled) acc = ibha_hash_mix(acc, ibha_xxh3_64(window, filled));
    return ibha_hash_final(acc, logical);
}
