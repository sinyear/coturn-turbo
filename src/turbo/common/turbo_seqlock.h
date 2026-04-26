/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Seqlock — concurrent read / exclusive write without reader blocking.
 * Design doc §T2.7 — protects the Allocation table that the libevent
 * thread writes and the Turbo Worker thread reads on the fast path.
 *
 * Properties:
 *   - Writers acquire a spin-lock (single writer expected).
 *   - Readers optimistically snapshot; retry if a write raced them.
 *   - Zero reader overhead when no write is in progress.
 *   - Readers never block writers.
 *
 * Typical writer usage:
 *   turbo_seqlock_write_begin(&sl);
 *   ... modify protected data ...
 *   turbo_seqlock_write_end(&sl);
 *
 * Typical reader usage:
 *   uint64_t seq;
 *   do {
 *       seq = turbo_seqlock_read_begin(&sl);
 *       ... read protected data into local copy ...
 *   } while (turbo_seqlock_read_retry(&sl, seq));
 *   ... use local copy ...
 */

#ifndef TURBO_SEQLOCK_H
#define TURBO_SEQLOCK_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdalign.h>

struct turbo_seqlock {
    _Atomic uint64_t seq;   /* odd = write in progress */
};

/* ------------------------------------------------------------------ */
/* Writer side                                                          */
/* ------------------------------------------------------------------ */

/*
 * Begin an exclusive write section.
 * Increments the sequence counter to an odd value (signals write-in-progress).
 * Spin-waits if another writer is already active (should never happen with a
 * single writer, but guards against bugs).
 */
static inline void turbo_seqlock_write_begin(struct turbo_seqlock *sl) {
    uint64_t seq;
    /* Spin until seq is even (no writer holding it) */
    do {
        seq = atomic_load_explicit(&sl->seq, memory_order_relaxed);
    } while ((seq & 1) ||
             !atomic_compare_exchange_weak_explicit(
                 &sl->seq, &seq, seq + 1,
                 memory_order_release,   /* publish the odd value */
                 memory_order_relaxed));
    /* Full barrier: the write to seq must complete before data writes */
    atomic_thread_fence(memory_order_seq_cst);
}

/*
 * End an exclusive write section.
 * Increments the sequence counter back to even, publishing the update.
 */
static inline void turbo_seqlock_write_end(struct turbo_seqlock *sl) {
    /* Ensure all data stores finish before the seq increment */
    atomic_thread_fence(memory_order_release);
    uint64_t cur = atomic_load_explicit(&sl->seq, memory_order_relaxed);
    atomic_store_explicit(&sl->seq, cur + 1, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Reader side                                                          */
/* ------------------------------------------------------------------ */

/*
 * Begin an optimistic read section.
 * Returns the current sequence snapshot; caller should copy protected data,
 * then call turbo_seqlock_read_retry() to verify.
 * Spins if a write is in progress (seq is odd) — this is the only blocking
 * behaviour for readers, and it is bounded by the write duration.
 */
static inline uint64_t turbo_seqlock_read_begin(const struct turbo_seqlock *sl) {
    uint64_t seq;
    do {
        seq = atomic_load_explicit(&sl->seq, memory_order_acquire);
    } while (seq & 1);   /* odd → write in progress; spin briefly */
    return seq;
}

/*
 * Verify that no write raced the read.
 * Returns non-zero (true) if the caller must retry the read.
 */
static inline int turbo_seqlock_read_retry(const struct turbo_seqlock *sl,
                                           uint64_t seq) {
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&sl->seq, memory_order_relaxed) != seq;
}

/* ------------------------------------------------------------------ */
/* Initialisation                                                       */
/* ------------------------------------------------------------------ */

static inline void turbo_seqlock_init(struct turbo_seqlock *sl) {
    atomic_store_explicit(&sl->seq, 0, memory_order_relaxed);
}

#endif /* TURBO_SEQLOCK_H */
