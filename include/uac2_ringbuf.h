/**
 * @file uac2_ringbuf.h
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) Audio Ring Buffer
 */

#ifndef __UAC2_RINGBUF_H
#define __UAC2_RINGBUF_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define UAC2_RINGBUF_CAPACITY (64 * 1024) /* 64KB: ~85ms buffer at 192kHz/24bit */

typedef struct {
    uint8_t buffer[UAC2_RINGBUF_CAPACITY];
    volatile uint32_t head; /* Written by USB ISR (Producer) */
    volatile uint32_t tail; /* Read by Audio DMA (Consumer) */
    uint32_t underrun_count;
    uint32_t overrun_count;
} Uac2RingBuffer;

static inline void uac2_ringbuf_init(Uac2RingBuffer *rb)
{
    memset(rb, 0, sizeof(Uac2RingBuffer));
}

static inline uint32_t uac2_ringbuf_available_read(const Uac2RingBuffer *rb)
{
    uint32_t h = rb->head;
    uint32_t t = rb->tail;
    if (h >= t) return h - t;
    return UAC2_RINGBUF_CAPACITY - (t - h);
}

static inline uint32_t uac2_ringbuf_available_write(const Uac2RingBuffer *rb)
{
    return (UAC2_RINGBUF_CAPACITY - 1u) - uac2_ringbuf_available_read(rb);
}

static inline uint32_t uac2_ringbuf_write(Uac2RingBuffer *rb, const uint8_t *data, uint32_t len)
{
    uint32_t avail = uac2_ringbuf_available_write(rb);
    if (len > avail) {
        rb->overrun_count++;
        len = avail;
    }
    if (len == 0) return 0;

    uint32_t h = rb->head;
    uint32_t first_chunk = UAC2_RINGBUF_CAPACITY - h;
    if (len <= first_chunk) {
        memcpy(&rb->buffer[h], data, len);
        rb->head = (h + len) & (UAC2_RINGBUF_CAPACITY - 1u);
    } else {
        memcpy(&rb->buffer[h], data, first_chunk);
        memcpy(&rb->buffer[0], data + first_chunk, len - first_chunk);
        rb->head = len - first_chunk;
    }
    return len;
}

static inline uint32_t uac2_ringbuf_read(Uac2RingBuffer *rb, uint8_t *dest, uint32_t len)
{
    uint32_t avail = uac2_ringbuf_available_read(rb);
    if (len > avail) {
        rb->underrun_count++;
        /* Fill remainder with zero (silence) */
        memset(dest + avail, 0, len - avail);
        len = avail;
    }
    if (len == 0) return 0;

    uint32_t t = rb->tail;
    uint32_t first_chunk = UAC2_RINGBUF_CAPACITY - t;
    if (len <= first_chunk) {
        memcpy(dest, &rb->buffer[t], len);
        rb->tail = (t + len) & (UAC2_RINGBUF_CAPACITY - 1u);
    } else {
        memcpy(dest, &rb->buffer[t], first_chunk);
        memcpy(dest + first_chunk, &rb->buffer[0], len - first_chunk);
        rb->tail = len - first_chunk;
    }
    return len;
}

#endif /* __UAC2_RINGBUF_H */
