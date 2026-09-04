/*
 * sys_wasm.c - system layer for the freestanding wasm32 build.
 *
 * There is no libc here. Memory comes from the WebAssembly memory instance via
 * memory.grow, one page at a time. Individual frees are no-ops: the engine's
 * allocation discipline is arena based and a context releases everything at once,
 * so a real free list would buy nothing and cost code size.
 *
 * The host is expected to instantiate one module per diff, or to call
 * ibha_csvd_ctx_free and accept that the linear memory high water mark persists
 * for the life of the instance. Both are documented in the JS binding.
 */
#ifdef __wasm__

#include "internal.h"

#define WASM_PAGE 65536u

/*
 * The ownership rule, which is the whole of the host contract:
 *
 *   **The engine owns exactly the pages it grew itself, and nothing else.**
 *
 * It is stated that way round deliberately. The obvious implementation starts at
 * __heap_base and treats everything up to the current memory size as free, and
 * that is wrong the moment there is a host: a JavaScript binding has to put the
 * CSV bytes somewhere, it does that with memory.grow, and an engine that assumes
 * it owns everything above __heap_base will hand those same addresses out again.
 * The corruption that follows is silent and looks like a parser bug.
 *
 * Growing only into pages we asked for costs the gap between __heap_base and the
 * initial memory size, which is under a megabyte and is the stack's neighbourhood
 * anyway. In exchange, host and engine allocations can never overlap: memory.grow
 * hands each caller a fresh disjoint range, whoever calls it.
 */
static uint8_t *g_brk;      /* next free byte in the region we grew */
static uint8_t *g_brk_end;  /* end of that region */

static size_t align_up_sz(size_t n, size_t a) { return (n + (a - 1)) & ~(a - 1); }

void *ibha_csvd_sys_alloc(size_t n) {
    n = align_up_sz(n, 16);
    if (n == 0) return NULL;

    if (g_brk == 0 || (size_t)(g_brk_end - g_brk) < n) {
        /* Whatever is left in the old region is abandoned rather than tracked.
         * The caller is an arena that asks for large blocks and never frees, so a
         * free list here would cost code size to reclaim a rounding error. */
        size_t pages = (n + WASM_PAGE - 1) / WASM_PAGE;
        size_t prev = __builtin_wasm_memory_grow(0, pages);
        if (prev == (size_t)-1) return NULL;
        g_brk = (uint8_t *)(prev * WASM_PAGE);
        g_brk_end = g_brk + pages * WASM_PAGE;
    }

    uint8_t *p = g_brk;
    g_brk += n;
    return p;
}

void ibha_csvd_sys_free(void *p, size_t n) {
    /* Bump allocator: reclaim only if this was the most recent allocation, which
     * makes the common ingest-then-free case cheap without a free list. */
    if (p && (uint8_t *)p + align_up_sz(n, 16) == g_brk) g_brk = (uint8_t *)p;
}

/* ---- routines the compiler emits calls to, and that we use directly -------- */

void *ibha_csvd_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *ibha_csvd_memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

int ibha_csvd_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t ibha_csvd_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * clang lowers struct assignment and some loops to these names regardless of
 * -nostdlib, so they must exist with external linkage.
 *
 * They are declared before they are defined because there is no libc header here
 * to have declared them, and a definition with external linkage and no prior
 * declaration is what -Wmissing-prototypes is for: it is usually a symbol that
 * should have been static. These three genuinely should not be.
 */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);

void *memcpy(void *dst, const void *src, size_t n) { return ibha_csvd_memcpy(dst, src, n); }
void *memset(void *dst, int c, size_t n) { return ibha_csvd_memset(dst, c, n); }
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

#endif /* __wasm__ */
