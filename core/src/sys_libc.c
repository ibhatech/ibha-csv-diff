/*
 * sys_libc.c - system memory layer for native builds.
 */
#ifndef __wasm__

#include <stdlib.h>

#include "internal.h"

void *ibha_csvd_sys_alloc(size_t n) { return malloc(n); }

void ibha_csvd_sys_free(void *p, size_t n) {
    (void)n; /* free() knows the size; the parameter exists for the wasm impl. */
    free(p);
}

#endif /* !__wasm__ */
