#pragma once
#include <stdint.h>
#include <string.h>

// Power-of-2 size so index masking works correctly
#define RING_BUF_SIZE (32 * 1024)

typedef struct {
    uint8_t  buf[RING_BUF_SIZE];
    uint32_t head;  // write cursor (never masked)
    uint32_t tail;  // read  cursor (never masked)
} ring_buf_t;

static inline void ring_buf_init(ring_buf_t *rb) {
    rb->head = rb->tail = 0;
}

static inline uint32_t ring_buf_available(const ring_buf_t *rb) {
    return rb->head - rb->tail;  // unsigned wrap-around gives correct result
}

static inline uint32_t ring_buf_free(const ring_buf_t *rb) {
    return RING_BUF_SIZE - ring_buf_available(rb);
}

// Write up to len bytes; returns bytes actually written (drops if full).
static inline uint32_t ring_buf_write(ring_buf_t *rb, const uint8_t *data, uint32_t len) {
    uint32_t space = ring_buf_free(rb);
    if (len > space) len = space;

    uint32_t head_idx = rb->head & (RING_BUF_SIZE - 1);
    uint32_t first    = RING_BUF_SIZE - head_idx;
    if (first > len) first = len;

    memcpy(rb->buf + head_idx, data, first);
    if (len > first)
        memcpy(rb->buf, data + first, len - first);

    rb->head += len;
    return len;
}

// Copy len bytes to dst without advancing the tail (non-destructive peek).
static inline uint32_t ring_buf_peek(ring_buf_t *rb, uint8_t *dst, uint32_t len) {
    uint32_t avail = ring_buf_available(rb);
    if (len > avail) len = avail;

    uint32_t tail_idx = rb->tail & (RING_BUF_SIZE - 1);
    uint32_t first    = RING_BUF_SIZE - tail_idx;
    if (first > len) first = len;

    memcpy(dst, rb->buf + tail_idx, first);
    if (len > first)
        memcpy(dst + first, rb->buf, len - first);

    return len;
}

// Advance the tail (mark bytes as consumed after a successful peek/decode).
static inline void ring_buf_consume(ring_buf_t *rb, uint32_t len) {
    rb->tail += len;
}
