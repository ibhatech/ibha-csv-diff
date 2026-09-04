#include "internal.h"

static size_t align_up(size_t n, size_t a) { return (n + (a - 1)) & ~(a - 1); }

void ibha_arena_init(ibha_arena *a) {
    a->head = NULL;
    a->reserved = 0;
    a->used = 0;
}

void ibha_arena_destroy(ibha_arena *a) {
    ibha_arena_block *b = a->head;
    while (b) {
        ibha_arena_block *next = b->next;
        /* The block header and its payload are one allocation. */
        ibha_csvd_sys_free(b, sizeof(*b) + b->cap);
        b = next;
    }
    ibha_arena_init(a);
}

/* Adds a block big enough for `need` bytes. Block sizes double up to
 * IBHA_ARENA_MAX_BLOCK so that a large ingest does not make thousands of calls
 * into the system allocator, while a 1 KB diff does not reserve megabytes. */
static int arena_grow(ibha_arena *a, size_t need) {
    size_t want = a->head ? a->head->cap * 2 : IBHA_ARENA_FIRST_BLOCK;
    if (want > IBHA_ARENA_MAX_BLOCK) want = IBHA_ARENA_MAX_BLOCK;
    if (want < need) want = align_up(need, IBHA_ARENA_ALIGN);

    ibha_arena_block *b = (ibha_arena_block *)ibha_csvd_sys_alloc(sizeof(*b) + want);
    if (!b) return 0;

    b->next = a->head;
    b->cap = want;
    b->used = 0;
    b->data = (uint8_t *)b + sizeof(*b);
    a->head = b;
    a->reserved += (uint64_t)(sizeof(*b) + want);
    return 1;
}

void *ibha_arena_alloc(ibha_arena *a, size_t n) {
    if (n == 0) n = 1;
    n = align_up(n, IBHA_ARENA_ALIGN);

    /* Guard against a size_t overflow in align_up on absurd inputs. */
    if (n < IBHA_ARENA_ALIGN) return NULL;

    if (!a->head || a->head->cap - a->head->used < n) {
        if (!arena_grow(a, n)) return NULL;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    a->used += (uint64_t)n;
    return p;
}

void *ibha_arena_alloc_large(ibha_arena *a, size_t n) {
    if (n == 0) n = 1;
    n = align_up(n, IBHA_ARENA_ALIGN);
    if (n < IBHA_ARENA_ALIGN) return NULL; /* align_up wrapped */

    /* Small requests gain nothing from a dedicated block and would fragment the
     * arena into thousands of them. */
    if (n < IBHA_ARENA_FIRST_BLOCK) return ibha_arena_alloc(a, n);

    ibha_arena_block *b = (ibha_arena_block *)ibha_csvd_sys_alloc(sizeof(*b) + n);
    if (!b) return NULL;

    b->cap = n;
    b->used = n;
    b->data = (uint8_t *)b + sizeof(*b);

    /* Linked in behind the head rather than in front of it, so the head keeps
     * serving small allocations out of whatever space it has left. */
    if (a->head) {
        b->next = a->head->next;
        a->head->next = b;
    } else {
        b->next = NULL;
        a->head = b;
    }

    a->reserved += (uint64_t)(sizeof(*b) + n);
    a->used += (uint64_t)n;
    return b->data;
}

void *ibha_arena_calloc(ibha_arena *a, size_t n) {
    void *p = ibha_arena_alloc(a, n);
    if (p) IBHA_MEMSET(p, 0, n);
    return p;
}

int ibha_bytebuf_reserve(ibha_arena *a, ibha_bytebuf *b, size_t need) {
    if (b->cap >= need) return 1;

    size_t want = b->cap ? b->cap : IBHA_ARENA_FIRST_BLOCK;
    while (want < need) {
        /* Overflow guard: a doubling that wraps means the request is impossible. */
        if (want > (size_t)-1 / 2) {
            want = need;
            break;
        }
        want *= 2;
    }

    /* A dedicated block: the buffer is usually the largest single thing in the
     * arena, and rounding it up to a block boundary would abandon the difference
     * on every doubling. */
    uint8_t *fresh = (uint8_t *)ibha_arena_alloc_large(a, want);
    if (!fresh) return 0;
    if (b->len) IBHA_MEMCPY(fresh, b->bytes, b->len);
    b->bytes = fresh;
    b->cap = want;
    return 1;
}

int ibha_bytebuf_append(ibha_arena *a, ibha_bytebuf *b, const uint8_t *src, size_t n) {
    if (n == 0) return 1;
    if (b->len > (size_t)-1 - n) return 0;
    if (!ibha_bytebuf_reserve(a, b, b->len + n)) return 0;
    IBHA_MEMCPY(b->bytes + b->len, src, n);
    b->len += n;
    return 1;
}
