#ifndef TURBO_HASH_H
#define TURBO_HASH_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/**
 * Lock-free hash table entry (chained)
 */
struct turbo_hash_entry {
    uint32_t key_hash;
    void *key;
    size_t key_len;
    void *value;
    uint8_t marked_for_delete;
    struct turbo_hash_entry *next;
};

/**
 * Simple chained hash table
 * Not truly lock-free, but uses RCU-style deferred deletion
 * for safe concurrent reads.
 */
struct turbo_hash {
    struct turbo_hash_entry **buckets;
    uint32_t num_buckets;
    uint32_t count;
    uint32_t max_entries;
    uint32_t seed;
    pthread_mutex_t write_lock;
};

/**
 * Hash function (FNV-1a variant)
 * @param key Key data
 * @param len Key length
 * @param seed Hash seed
 * @return Hash value
 */
uint32_t turbo_hash_fn(const void *key, size_t len, uint32_t seed);

/**
 * Initialize hash table
 * @param h Hash table
 * @param max_entries Maximum entries
 * @param seed Hash seed (0 for default)
 * @return 0 on success, negative error code on failure
 */
int turbo_hash_init(struct turbo_hash *h, uint32_t max_entries, uint32_t seed);

/**
 * Cleanup hash table
 * @param h Hash table
 */
void turbo_hash_cleanup(struct turbo_hash *h);

/**
 * Insert key-value pair
 * @param h Hash table
 * @param key Key data
 * @param key_len Key length
 * @param value Value pointer
 * @return 0 on success, negative error code on failure
 */
int turbo_hash_insert(struct turbo_hash *h, const void *key, size_t key_len, void *value);

/**
 * Lookup value by key (lock-free read)
 * @param h Hash table
 * @param key Key data
 * @param key_len Key length
 * @return Value pointer or NULL
 */
void* turbo_hash_lookup(struct turbo_hash *h, const void *key, size_t key_len);

/**
 * Remove entry by key (write path)
 * @param h Hash table
 * @param key Key data
 * @param key_len Key length
 */
void turbo_hash_remove(struct turbo_hash *h, const void *key, size_t key_len);

/**
 * Get entry count
 * @param h Hash table
 * @return Number of entries
 */
uint32_t turbo_hash_count(struct turbo_hash *h);

#endif /* TURBO_HASH_H */
