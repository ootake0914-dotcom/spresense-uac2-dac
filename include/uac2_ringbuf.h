/**
 * @file uac2_ringbuf.h
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) Audio Ring Buffer
 *
 * Rev84 (1): head/tail are C11 atomics with acquire/release ordering.
 * On this single-core build that specifically guarantees publish order:
 * producer memcpy-then-head(release), consumer memcpy-then-tail(release),
 * with acquire loads on the far side. volatile alone does not order the
 * payload memcpy against the index store. (Also SMP-ready if that ever
 * comes back.)
 */

#ifndef __UAC2_RINGBUF_H
#define __UAC2_RINGBUF_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#define UAC2_RINGBUF_CAPACITY (128 * 1024) /* 128KB: ~85ms @192k/stereo/32-bit.
 * 1.5MB RAM制約のため256KBは不可。死にringbuf削除と等価交換で差分ゼロ。
 * 壁2倍＋レベルサーボで無限化するので十分。 */

typedef struct {
    uint8_t buffer[UAC2_RINGBUF_CAPACITY];
    atomic_uint head; /* Written by USB ISR (Producer), release-store */
    atomic_uint tail; /* Read by Audio DMA (Consumer), release-store */
    uint32_t underrun_count; /* stats: single-writer each, telemetry-only */
    uint32_t overrun_count;
} Uac2RingBuffer;

static inline void uac2_ringbuf_init(Uac2RingBuffer *rb)
{
    /* Zero-init is a valid init for the atomics (pre-thread, no race).
     * memset kept (also clears buffer + stats in one shot).
     */
    memset(rb, 0, sizeof(Uac2RingBuffer));
}

static inline uint32_t uac2_ringbuf_available_read(const Uac2RingBuffer *rb)
{
    uint32_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&rb->tail, memory_order_acquire);
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
        /* CRITICAL: Align to 8-byte stereo frame boundary.
         * Writing a partial frame destroys PCM byte-alignment forever,
         * turning all subsequent audio into harsh noise until stream restart.
         */
        len = avail & ~7u;
    }
    if (len == 0) return 0;

    uint32_t h = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint32_t first_chunk = UAC2_RINGBUF_CAPACITY - h;
    if (len <= first_chunk) {
        memcpy(&rb->buffer[h], data, len);
        atomic_store_explicit(&rb->head,
                              (h + len) & (UAC2_RINGBUF_CAPACITY - 1u),
                              memory_order_release);
    } else {
        memcpy(&rb->buffer[h], data, first_chunk);
        memcpy(&rb->buffer[0], data + first_chunk, len - first_chunk);
        atomic_store_explicit(&rb->head, len - first_chunk,
                              memory_order_release);
    }
    return len;
}

static inline uint32_t uac2_ringbuf_read(Uac2RingBuffer *rb, uint8_t *dest, uint32_t len)
{
    uint32_t avail = uac2_ringbuf_available_read(rb);
    if (len > avail) {
        rb->underrun_count++;
        /* Fill remainder with zero (silence) */
        uint32_t aligned_avail = avail & ~7u;
        memset(dest + aligned_avail, 0, len - aligned_avail);
        len = aligned_avail;
    }
    if (len == 0) return 0;

    uint32_t t = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t first_chunk = UAC2_RINGBUF_CAPACITY - t;
    if (len <= first_chunk) {
        memcpy(dest, &rb->buffer[t], len);
        atomic_store_explicit(&rb->tail,
                              (t + len) & (UAC2_RINGBUF_CAPACITY - 1u),
                              memory_order_release);
    } else {
        memcpy(dest, &rb->buffer[t], first_chunk);
        memcpy(dest + first_chunk, &rb->buffer[0], len - first_chunk);
        atomic_store_explicit(&rb->tail, len - first_chunk,
                              memory_order_release);
    }
    return len;
}

/* Flush (drop) all buffered content: tail <- head. Used at stream
 * boundaries for stale residue. Single call site (pump, task context).
 */
static inline void uac2_ringbuf_flush(Uac2RingBuffer *rb)
{
    uint32_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
    atomic_store_explicit(&rb->tail, h, memory_order_release);
}

#endif /* __UAC2_RINGBUF_H */
