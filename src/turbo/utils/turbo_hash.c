#include "turbo_hash.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define HASH_BUCKETS_BASE 1024

/* FNV-1a hash */
uint32_t turbo_hash_fn(const void *key, size_t len, uint32_t seed) {
    const uint8_t *d = (const uint8_t *)key;
    uint32_t hash = seed ^ 0x811c9dc5;

    for (size_t i = 0; i < len; i++) {
        hash ^= d[i];
        hash *= 0x01000193;
    }

    return hash;
}

static struct turbo_hash_entry* entry_alloc(void) {
    return (struct turbo_hash_entry *)calloc(1, sizeof(struct turbo_hash_entry));
}

static void entry_free(struct turbo_hash_entry *e) {
    if (e) {
        free(e->key);
        free(e);
    }
}

int turbo_hash_init(struct turbo_hash *h, uint32_t max_entries, uint32_t seed) {
    if (!h || max_entries == 0) {
        return -EINVAL;
    }

    memset(h, 0, sizeof(*h));
    h->num_buckets = HASH_BUCKETS_BASE;
    h->max_entries = max_entries;
    h->seed = seed ? seed : 0xdeadbeef;
    h->buckets = (struct turbo_hash_entry **)calloc(h->num_buckets, sizeof(struct turbo_hash_entry *));
    if (!h->buckets) {
        return -ENOMEM;
    }

    if (pthread_mutex_init(&h->write_lock, NULL) != 0) {
        free(h->buckets);
        h->buckets = NULL;
        return -EIO;
    }

    return 0;
}

void turbo_hash_cleanup(struct turbo_hash *h) {
    if (!h || !h->buckets) {
        return;
    }

    pthread_mutex_lock(&h->write_lock);
    for (uint32_t i = 0; i < h->num_buckets; i++) {
        struct turbo_hash_entry *e = h->buckets[i];
        while (e) {
            struct turbo_hash_entry *next = e->next;
            entry_free(e);
            e = next;
        }
    }
    free(h->buckets);
    h->buckets = NULL;
    pthread_mutex_unlock(&h->write_lock);
    pthread_mutex_destroy(&h->write_lock);
}

int turbo_hash_insert(struct turbo_hash *h, const void *key, size_t key_len, void *value) {
    if (!h || !key || key_len == 0 || !value || !h->buckets) {
        return -EINVAL;
    }

    pthread_mutex_lock(&h->write_lock);

    if (h->count >= h->max_entries) {
        pthread_mutex_unlock(&h->write_lock);
        return -ENOSPC;
    }

    uint32_t hash = turbo_hash_fn(key, key_len, h->seed);
    uint32_t bucket = hash % h->num_buckets;

    /* Check for existing key */
    struct turbo_hash_entry *e = h->buckets[bucket];
    while (e) {
        if (!e->marked_for_delete && e->key_hash == hash &&
            e->key_len == key_len &&
            memcmp(e->key, key, key_len) == 0) {
            /* Update existing value */
            e->value = value;
            pthread_mutex_unlock(&h->write_lock);
            return 0;
        }
        e = e->next;
    }

    /* Allocate new entry */
    e = entry_alloc();
    if (!e) {
        pthread_mutex_unlock(&h->write_lock);
        return -ENOMEM;
    }

    e->key = malloc(key_len);
    if (!e->key) {
        free(e);
        pthread_mutex_unlock(&h->write_lock);
        return -ENOMEM;
    }

    memcpy(e->key, key, key_len);
    e->key_len = key_len;
    e->key_hash = hash;
    e->value = value;

    /* Insert at head */
    e->next = h->buckets[bucket];
    h->buckets[bucket] = e;
    h->count++;

    pthread_mutex_unlock(&h->write_lock);
    return 0;
}

void* turbo_hash_lookup(struct turbo_hash *h, const void *key, size_t key_len) {
    if (!h || !key || key_len == 0 || !h->buckets) {
        return NULL;
    }

    uint32_t hash = turbo_hash_fn(key, key_len, h->seed);
    uint32_t bucket = hash % h->num_buckets;

    struct turbo_hash_entry *e = h->buckets[bucket];
    while (e) {
        if (!e->marked_for_delete && e->key_hash == hash &&
            e->key_len == key_len &&
            memcmp(e->key, key, key_len) == 0) {
            return e->value;
        }
        e = e->next;
    }

    return NULL;
}

void turbo_hash_remove(struct turbo_hash *h, const void *key, size_t key_len) {
    if (!h || !key || key_len == 0 || !h->buckets) {
        return;
    }

    pthread_mutex_lock(&h->write_lock);

    uint32_t hash = turbo_hash_fn(key, key_len, h->seed);
    uint32_t bucket = hash % h->num_buckets;

    struct turbo_hash_entry *e = h->buckets[bucket];
    struct turbo_hash_entry *prev = NULL;

    while (e) {
        if (!e->marked_for_delete && e->key_hash == hash &&
            e->key_len == key_len &&
            memcmp(e->key, key, key_len) == 0) {
            /* Mark for deletion */
            e->marked_for_delete = 1;

            /* Unlink */
            if (prev) {
                prev->next = e->next;
            } else {
                h->buckets[bucket] = e->next;
            }
            h->count--;

            entry_free(e);
            break;
        }
        prev = e;
        e = e->next;
    }

    pthread_mutex_unlock(&h->write_lock);
}

uint32_t turbo_hash_count(struct turbo_hash *h) {
    if (!h) return 0;
    return h->count;
}
