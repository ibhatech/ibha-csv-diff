/*
 * tap.h - minimal TAP style assertions. No dependencies, no framework.
 *
 * Deliberately tiny: the real correctness strategy for this engine is the golden
 * corpus, the property tests and the fuzzer described in spec section 10. These
 * unit assertions exist to keep the plumbing honest, not to be the safety net.
 */
#ifndef IBHA_TAP_H
#define IBHA_TAP_H

#include <stdio.h>
#include <string.h>

/* The counters live in whichever translation unit defines TAP_MAIN, so that
 * suites can be split across files and still produce one numbered TAP stream. */
extern int tap_ran;
extern int tap_failed;

#ifdef TAP_MAIN
int tap_ran = 0;
int tap_failed = 0;
#endif

#define TAP_OK(cond, name)                                              \
    do {                                                               \
        tap_ran++;                                                     \
        if (cond) {                                                    \
            printf("ok %d - %s\n", tap_ran, (name));                   \
        } else {                                                       \
            tap_failed++;                                              \
            printf("not ok %d - %s\n", tap_ran, (name));               \
            printf("  at %s:%d\n", __FILE__, __LINE__);                \
        }                                                              \
    } while (0)

#define TAP_EQ_U64(got, want, name)                                     \
    do {                                                               \
        unsigned long long g_ = (unsigned long long)(got);              \
        unsigned long long w_ = (unsigned long long)(want);             \
        tap_ran++;                                                     \
        if (g_ == w_) {                                                \
            printf("ok %d - %s\n", tap_ran, (name));                   \
        } else {                                                       \
            tap_failed++;                                              \
            printf("not ok %d - %s\n", tap_ran, (name));               \
            printf("  got %llu want %llu at %s:%d\n", g_, w_, __FILE__, __LINE__); \
        }                                                              \
    } while (0)

#define TAP_EQ_STR(got, want, name)                                     \
    do {                                                               \
        const char *g_ = (got);                                        \
        const char *w_ = (want);                                       \
        tap_ran++;                                                     \
        if (g_ && w_ && strcmp(g_, w_) == 0) {                         \
            printf("ok %d - %s\n", tap_ran, (name));                   \
        } else {                                                       \
            tap_failed++;                                              \
            printf("not ok %d - %s\n", tap_ran, (name));               \
            printf("  got \"%s\" want \"%s\" at %s:%d\n",              \
                   g_ ? g_ : "(null)", w_ ? w_ : "(null)", __FILE__, __LINE__); \
        }                                                              \
    } while (0)

#define TAP_DONE()                                                      \
    do {                                                               \
        printf("1..%d\n", tap_ran);                                    \
        if (tap_failed) fprintf(stderr, "FAILED %d of %d\n", tap_failed, tap_ran); \
        return tap_failed ? 1 : 0;                                     \
    } while (0)

#endif /* IBHA_TAP_H */
