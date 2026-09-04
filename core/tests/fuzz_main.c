/*
 * fuzz_main.c - a standalone driver for the libFuzzer targets.
 *
 * Apple's bundled clang ships no libFuzzer runtime, so `make fuzz` cannot link
 * on a stock macOS machine. Leaving the parser's only untrusted input surface
 * unfuzzed until someone installs a second toolchain is not an acceptable
 * answer, so this driver runs the same LLVMFuzzerTestOneInput entry point with
 * its own generator. It has no coverage feedback and is therefore weaker than
 * libFuzzer, but it is deterministic, dependency free, runs under ASan and UBSan
 * everywhere, and finds the shallow bugs immediately.
 *
 *   make fuzz-native
 *   build/fuzz_parse_native 200000 12345
 *
 * The generator is biased hard toward the bytes that drive the state machine, so
 * that a random input is a plausible CSV rather than noise the parser rejects in
 * its first byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Structurally interesting seeds. Mutations of these reach the states that
 * uniformly random bytes would take a long time to stumble into. */
static const char *const k_seeds[] = {
    "a,b,c\n1,2,3\n",
    "a,b\r\n1,2\r\n",
    "a,b\r1,2\r",
    /* Split so the hex escape cannot swallow the 'a' as a fourth hex digit. */
    ("\xEF\xBB\xBF"
     "a,b\n1,2\n"),
    "a,b\n\"x\"\"y\",2\n",
    "a,b\n\"line\none\",2\n",
    "a,b\n\"unterminated,2\n",
    "a,b\n\"ab\" ,2\n",
    "a,b\n\"ab\"c,2\n",
    "a,b,c\n1,2\n",
    "a,b,c\n1,2,3,,\n",
    "KEY,,\nREQUIRED,,\nINT,INT,INT\nid,name,qty\n1,2,3\n",
    "a\n\n\n\"\"\n",
    "a,b\n,\n",
    ",,,\n,,,\n",
};

#define SEED_COUNT ((int)(sizeof(k_seeds) / sizeof(k_seeds[0])))

/* The bytes that matter, over-represented on purpose. */
static const char k_alphabet[] = "\",\r\n\t abc123\"\",\n\xEF\xBB\xBF\xFF";

static uint64_t rng_state;

static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static size_t gen(uint8_t *buf, size_t cap) {
    size_t len = 0;
    int mode = (int)(rnd() % 3u);

    if (mode == 0) {
        /* A seed, lightly corrupted. */
        const char *seed = k_seeds[rnd() % (uint32_t)SEED_COUNT];
        len = strlen(seed);
        if (len > cap) len = cap;
        memcpy(buf, seed, len);
        uint32_t edits = rnd() % 5u;
        for (uint32_t i = 0; i < edits && len; i++) {
            buf[rnd() % (uint32_t)len] =
                (uint8_t)k_alphabet[rnd() % (uint32_t)(sizeof(k_alphabet) - 1)];
        }
    } else if (mode == 1) {
        /* Two seeds spliced, which is how a chunk boundary bug in one construct
         * ends up next to another construct. */
        const char *a = k_seeds[rnd() % (uint32_t)SEED_COUNT];
        const char *b = k_seeds[rnd() % (uint32_t)SEED_COUNT];
        size_t la = strlen(a), lb = strlen(b);
        size_t cut = la ? rnd() % (uint32_t)la : 0;
        len = cut + lb;
        if (len > cap) len = cap;
        memcpy(buf, a, cut < len ? cut : len);
        if (cut < len) memcpy(buf + cut, b, len - cut);
    } else {
        /* Free form, from the biased alphabet. */
        len = rnd() % (cap < 512 ? cap : 512);
        for (size_t i = 0; i < len; i++) {
            buf[i] = (uint8_t)k_alphabet[rnd() % (uint32_t)(sizeof(k_alphabet) - 1)];
        }
    }

    /* The first two bytes steer the harness, not the parser, so they are always
     * randomized: chunk stride and options selector. */
    if (len < 3) len = 3;
    buf[0] = (uint8_t)(rnd() & 0xFFu);
    buf[1] = (uint8_t)(rnd() & 0xFFu);
    return len;
}

int main(int argc, char **argv) {
    long iterations = argc > 1 ? strtol(argv[1], NULL, 10) : 100000;
    rng_state = argc > 2 ? strtoull(argv[2], NULL, 10) : 0x243F6A8885A308D3ull;
    if (rng_state == 0) rng_state = 1;
    if (iterations < 1) iterations = 1;

    static uint8_t buf[8192];
    for (long i = 0; i < iterations; i++) {
        size_t len = gen(buf, sizeof(buf));
        LLVMFuzzerTestOneInput(buf, len);
    }

    printf("fuzz: %ld inputs, no invariant violated\n", iterations);
    return 0;
}
