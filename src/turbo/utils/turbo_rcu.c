#include "turbo_rcu.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define DEFAULT_MAX_PENDING 1024

int turbo_rcu_init(struct turbo_rcu *rcu, uint32_t max_pending) {
    if (!rcu) {
        return -EINVAL;
    }

    memset(rcu, 0, sizeof(*rcu));
    rcu->max_pending = max_pending ? max_pending : DEFAULT_MAX_PENDING;

    if (pthread_mutex_init(&rcu->reader_lock, NULL) != 0) {
        return -EIO;
    }
    if (pthread_mutex_init(&rcu->defer_lock, NULL) != 0) {
        pthread_mutex_destroy(&rcu->reader_lock);
        return -EIO;
    }

    return 0;
}

void turbo_rcu_cleanup(struct turbo_rcu *rcu) {
    if (!rcu) {
        return;
    }

    /* Force reclaim all pending entries */
    turbo_rcu_synchronize(rcu);

    pthread_mutex_destroy(&rcu->reader_lock);
    pthread_mutex_destroy(&rcu->defer_lock);
}

int turbo_rcu_defer_free(struct turbo_rcu *rcu, void *ptr) {
    if (!rcu || !ptr) {
        return -EINVAL;
    }

    struct turbo_rcu_entry *entry = (struct turbo_rcu_entry *)calloc(1, sizeof(*entry));
    if (!entry) {
        return -ENOMEM;
    }

    entry->ptr = ptr;

    pthread_mutex_lock(&rcu->defer_lock);

    /* Add to tail of pending list */
    if (rcu->pending_tail) {
        rcu->pending_tail->next = entry;
    } else {
        rcu->pending_head = entry;
    }
    rcu->pending_tail = entry;
    rcu->pending_count++;

    pthread_mutex_unlock(&rcu->defer_lock);

    return 0;
}

void turbo_rcu_synchronize(struct turbo_rcu *rcu) {
    if (!rcu) {
        return;
    }

    /* Wait for all active readers to exit */
    int max_spins = 1000;
    while (__atomic_load_n(&rcu->active_readers, __ATOMIC_SEQ_CST) > 0 && max_spins > 0) {
        usleep(100); /* 100us poll interval */
        max_spins--;
    }

    /* Free all pending entries */
    pthread_mutex_lock(&rcu->defer_lock);

    struct turbo_rcu_entry *entry = rcu->pending_head;
    while (entry) {
        struct turbo_rcu_entry *next = entry->next;
        free(entry->ptr);
        free(entry);
        entry = next;
    }

    rcu->total_freed += rcu->pending_count;
    rcu->pending_head = NULL;
    rcu->pending_tail = NULL;
    rcu->pending_count = 0;
    rcu->grace_period_count++;

    pthread_mutex_unlock(&rcu->defer_lock);
}

int turbo_rcu_maybe_reclaim(struct turbo_rcu *rcu, int force) {
    if (!rcu) {
        return 0;
    }

    if (force || rcu->pending_count >= rcu->max_pending) {
        turbo_rcu_synchronize(rcu);
        return 1;
    }

    return 0;
}

void turbo_rcu_stats(struct turbo_rcu *rcu, uint32_t *pending_count, uint64_t *total_freed) {
    if (!rcu) {
        return;
    }

    if (pending_count) {
        *pending_count = rcu->pending_count;
    }
    if (total_freed) {
        *total_freed = rcu->total_freed;
    }
}
