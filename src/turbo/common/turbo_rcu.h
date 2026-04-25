#ifndef TURBO_RCU_H
#define TURBO_RCU_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/**
 * Userspace RCU (Read-Copy-Update) for safe memory reclamation.
 *
 * Provides deferred free mechanism for lock-free readers.
 * When a writer removes an element, it is placed on a pending
 * list and only freed after all active readers have completed
 * their critical sections.
 *
 * Usage:
 *   1. turbo_rcu_init(&rcu)
 *   2. Reader: turbo_rcu_read_lock(&rcu) ... turbo_rcu_read_unlock(&rcu)
 *   3. Writer: turbo_rcu_defer_free(&rcu, ptr)
 *   4. Periodically: turbo_rcu_synchronize(&rcu) to reclaim memory
 */

/* Pending free entry */
struct turbo_rcu_entry {
    void *ptr;
    struct turbo_rcu_entry *next;
};

/* RCU state */
struct turbo_rcu {
    /* Reader tracking */
    int active_readers;
    pthread_mutex_t reader_lock;
    uint64_t grace_period_count;

    /* Deferred free list */
    struct turbo_rcu_entry *pending_head;
    struct turbo_rcu_entry *pending_tail;
    uint32_t pending_count;
    pthread_mutex_t defer_lock;

    /* Max pending entries before forced reclamation */
    uint32_t max_pending;

    /* Total freed entries (stats) */
    uint64_t total_freed;
};

/**
 * Initialize RCU state
 * @param rcu RCU state
 * @param max_pending Max pending entries before forced sync (0 = default 1024)
 * @return 0 on success, negative error code
 */
int turbo_rcu_init(struct turbo_rcu *rcu, uint32_t max_pending);

/**
 * Cleanup RCU state and free all pending entries
 * @param rcu RCU state
 */
void turbo_rcu_cleanup(struct turbo_rcu *rcu);

/**
 * Enter RCU read-side critical section
 * @param rcu RCU state
 */
static inline void turbo_rcu_read_lock(struct turbo_rcu *rcu) {
    if (!rcu) return;
    /* Memory barrier to ensure we see the latest grace period count */
    __atomic_fetch_add(&rcu->active_readers, 1, __ATOMIC_SEQ_CST);
}

/**
 * Exit RCU read-side critical section
 * @param rcu RCU state
 */
static inline void turbo_rcu_read_unlock(struct turbo_rcu *rcu) {
    if (!rcu) return;
    __atomic_fetch_sub(&rcu->active_readers, 1, __ATOMIC_SEQ_CST);
}

/**
 * Defer freeing of a pointer
 * The pointer will be reclaimed after all active readers exit.
 * @param rcu RCU state
 * @param ptr Pointer to free
 * @return 0 on success, negative error code
 */
int turbo_rcu_defer_free(struct turbo_rcu *rcu, void *ptr);

/**
 * Wait for a grace period (all active readers to exit)
 * and reclaim pending entries.
 * @param rcu RCU state
 */
void turbo_rcu_synchronize(struct turbo_rcu *rcu);

/**
 * Check if reclamation is needed and optionally trigger it.
 * @param rcu RCU state
 * @param force If true, force reclamation regardless of pending count
 * @return 1 if reclamation was performed, 0 otherwise
 */
int turbo_rcu_maybe_reclaim(struct turbo_rcu *rcu, int force);

/**
 * Get RCU stats
 * @param rcu RCU state
 * @param pending_count Output: number of pending frees
 * @param total_freed Output: total entries reclaimed
 */
void turbo_rcu_stats(struct turbo_rcu *rcu, uint32_t *pending_count, uint64_t *total_freed);

#endif /* TURBO_RCU_H */
