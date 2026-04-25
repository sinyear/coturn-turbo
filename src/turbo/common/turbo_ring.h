/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Single-Producer Single-Consumer (SPSC) lock-free ring queue.
 * Design doc §T2.6 — Worker thread → libevent thread STUN control path.
 *
 * Usage contract:
 *   - Exactly ONE producer thread calls turbo_ring_push().
 *   - Exactly ONE consumer thread calls turbo_ring_pop().
 *   - The ring size MUST be a power of two.
 *   - Items are void* pointers (cast to whatever the caller needs).
 *
 * Memory ordering: uses release on push, acquire on pop — guarantees the
 * consumer sees a fully-written item before the slot appears non-empty.
 */

#ifndef TURBO_RING_H
#define TURBO_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_RING_DEFAULT_SIZE 1024  /* power-of-two; must be ≥ 2 */

/* Pad each counter to its own cache line to avoid false sharing */
#define TURBO_CACHE_LINE 64

struct turbo_ring {
    /* Producer-side state (written only by producer) */
    alignas(TURBO_CACHE_LINE)
    _Atomic uint64_t head;      /* next slot to write into */

    /* Consumer-side state (written only by consumer) */
    alignas(TURBO_CACHE_LINE)
    _Atomic uint64_t tail;      /* next slot to read from */

    /* Read-only after init */
    alignas(TURBO_CACHE_LINE)
    uint64_t  mask;             /* size - 1; for fast modulo */
    void    **slots;            /* ring buffer (void* array) */
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

/*
 * Initialise a ring with the given capacity (must be a power of two).
 * Returns 0 on success, -1 on allocation failure.
 */
static inline int turbo_ring_init(struct turbo_ring *r, uint64_t size) {
    if (!r || size < 2 || (size & (size - 1)) != 0)
        return -1;   /* size must be a power of two */
    r->slots = calloc(size, sizeof(void *));
    if (!r->slots) return -1;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    r->mask = size - 1;
    return 0;
}

/*
 * Release ring memory.  Does NOT free any items still in the ring.
 */
static inline void turbo_ring_destroy(struct turbo_ring *r) {
    if (r && r->slots) {
        free(r->slots);
        r->slots = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Push / pop — inlined for zero call overhead on the hot path         */
/* ------------------------------------------------------------------ */

/*
 * Push one item from the producer thread.
 * Returns 1 on success, 0 if the ring is full.
 */
static inline int turbo_ring_push(struct turbo_ring *r, void *item) {
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (head - tail > r->mask)
        return 0;   /* full */

    r->slots[head & r->mask] = item;
    /* Release: the slot write must be visible before head advances */
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return 1;
}

/*
 * Pop one item from the consumer thread.
 * Returns the item on success, NULL if the ring is empty.
 */
static inline void *turbo_ring_pop(struct turbo_ring *r) {
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (tail == head)
        return NULL;   /* empty */

    void *item = r->slots[tail & r->mask];
    /* Release: tell the producer this slot is now free */
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return item;
}

/*
 * Drain up to `max` items into `out[]`.
 * Returns the number of items actually written.
 */
static inline int turbo_ring_drain(struct turbo_ring *r,
                                   void **out, int max) {
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    int n = 0;

    while (n < max && tail != head) {
        out[n++] = r->slots[tail & r->mask];
        tail++;
    }
    if (n > 0)
        atomic_store_explicit(&r->tail, tail, memory_order_release);
    return n;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                        */
/* ------------------------------------------------------------------ */

static inline uint64_t turbo_ring_size(const struct turbo_ring *r) {
    return r->mask + 1;
}

static inline int turbo_ring_empty(const struct turbo_ring *r) {
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    return tail == head;
}

static inline uint64_t turbo_ring_count(const struct turbo_ring *r) {
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    return head - tail;
}

#endif /* TURBO_RING_H */
