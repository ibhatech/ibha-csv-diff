/*
 * reader.c - adapters over the pull based byte source, and over the push based
 * byte sink the emitters write into. Both ends of the library are a callback the
 * caller supplies, so the library never opens a file, never performs a fetch and
 * never decides where output goes.
 */
#include "internal.h"

#ifndef __wasm__
#include <errno.h>
#include <unistd.h>
#endif

void ibha_csvd_buffer_reader_init(ibha_csvd_buffer_reader *r, const void *bytes, size_t len) {
    r->bytes = (const uint8_t *)bytes;
    r->len = len;
    r->pos = 0;
}

int64_t ibha_csvd_buffer_read(void *read_ctx, uint8_t *dst, size_t cap) {
    ibha_csvd_buffer_reader *r = (ibha_csvd_buffer_reader *)read_ctx;
    if (!r || !dst) return -1;

    size_t left = r->len - r->pos;
    if (left == 0) return 0;

    size_t n = left < cap ? left : cap;
    IBHA_MEMCPY(dst, r->bytes + r->pos, n);
    r->pos += n;
    return (int64_t)n;
}

/* ------------------------------------------------------------------ sinks -- */

void ibha_csvd_buffer_sink_init(ibha_csvd_buffer_sink *s, void *bytes, size_t cap) {
    s->bytes = (uint8_t *)bytes;
    s->cap = cap;
    s->len = 0;
    s->overflow = 0;
}

/*
 * Keeps counting past the end rather than failing, so len is always the size the
 * output would have had. A caller sizing a buffer for an HTML report wants the
 * number, not an error it has to bisect for.
 */
int ibha_csvd_buffer_sink_write(void *sink_ctx, const void *bytes, size_t len) {
    ibha_csvd_buffer_sink *s = (ibha_csvd_buffer_sink *)sink_ctx;
    if (!s) return -1;

    if (s->len < s->cap) {
        size_t room = s->cap - s->len;
        size_t take = len < room ? len : room;
        if (take && bytes) IBHA_MEMCPY(s->bytes + s->len, bytes, take);
        if (take < len) s->overflow = 1;
    } else if (len) {
        s->overflow = 1;
    }
    s->len += len;
    return 0;
}

/*
 * The one call that makes the emitters reachable from a host that cannot form a
 * C function pointer. See the header for why wasm32 makes that impossible from
 * JavaScript.
 */
void ibha_csvd_buffer_sink_bind(ibha_csvd_sink *sink, ibha_csvd_buffer_sink *s) {
    if (!sink) return;
    sink->write = ibha_csvd_buffer_sink_write;
    sink->ctx = s;
}

#ifndef __wasm__

void ibha_csvd_fd_reader_init(ibha_csvd_fd_reader *r, int fd) { r->fd = fd; }

int64_t ibha_csvd_fd_read(void *read_ctx, uint8_t *dst, size_t cap) {
    ibha_csvd_fd_reader *r = (ibha_csvd_fd_reader *)read_ctx;
    if (!r || !dst) return -1;

    for (;;) {
        ssize_t n = read(r->fd, dst, cap);
        if (n >= 0) return (int64_t)n;
        /* A signal during a large read is not an error worth failing a batch
         * over, so retry rather than surfacing it. */
        if (errno == EINTR) continue;
        return -1;
    }
}

void ibha_csvd_fd_sink_init(ibha_csvd_fd_sink *s, int fd) { s->fd = fd; }

int ibha_csvd_fd_sink_write(void *sink_ctx, const void *bytes, size_t len) {
    ibha_csvd_fd_sink *s = (ibha_csvd_fd_sink *)sink_ctx;
    const uint8_t *p = (const uint8_t *)bytes;
    if (!s) return -1;

    /* A short write is not an error, and a signal in the middle of one is not
     * either. Both are ordinary on a pipe, which is where a streaming emitter
     * usually points. */
    while (len) {
        ssize_t n = write(s->fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

#endif /* !__wasm__ */
