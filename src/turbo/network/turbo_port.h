#ifndef TURBO_PORT_H
#define TURBO_PORT_H

#include "turbo_netif.h"
#include "../utils/turbo_rcu.h"
#include <stdint.h>
#include <netinet/in.h>
#include <pthread.h>

/* Maximum address size for IPv6 */
#define TURBO_MAX_ADDR_LEN 16

/* Address family constants */
#define TURBO_AF_INET  4
#define TURBO_AF_INET6 6

/* Five-tuple key for session lookup */
struct turbo_five_tuple {
    uint8_t  src_addr[TURBO_MAX_ADDR_LEN];  /* Source IP (network byte order) */
    uint16_t src_port;                       /* Source port (network byte order) */
    uint8_t  dst_addr[TURBO_MAX_ADDR_LEN];   /* Dest IP (network byte order) */
    uint16_t dst_port;                       /* Dest port (network byte order) */
    uint8_t  proto;                          /* IPPROTO_UDP, etc. */
    uint8_t  af;                             /* TURBO_AF_INET or TURBO_AF_INET6 */
};

/* Session mapping entry: five-tuple -> room + member */
struct turbo_session {
    struct turbo_five_tuple key;
    uint32_t room_id;
    uint32_t member_id;
    uint32_t hash;        /* Cached hash value */
    uint8_t  marked_for_delete;
    struct turbo_session *next; /* Hash chain */
};

/* Port map: single-port multiplexing layer */
struct turbo_port_map {
    struct turbo_session **buckets;
    uint32_t num_buckets;
    uint32_t hash_seed;
    uint32_t count;
    uint32_t max_entries;
    pthread_mutex_t lock; /* Write lock; reads are lock-free via RCU-style iteration */
    struct turbo_rcu rcu; /* Userspace RCU for deferred deletion */
};

/**
 * Extract five-tuple from raw UDP packet (IPv4 or IPv6 + UDP headers)
 * @param data Packet data starting at IP header
 * @param len Packet length
 * @param tuple Output five-tuple
 * @return 0 on success, -1 on failure
 */
int turbo_extract_five_tuple(const uint8_t *data, size_t len, struct turbo_five_tuple *tuple);

/**
 * Initialize port map
 * @param pmap Port map to initialize
 * @param max_entries Maximum session entries
 * @return 0 on success, negative error code on failure
 */
int turbo_port_map_init(struct turbo_port_map *pmap, uint32_t max_entries);

/**
 * Cleanup port map
 * @param pmap Port map to cleanup
 */
void turbo_port_map_cleanup(struct turbo_port_map *pmap);

/**
 * Insert session mapping (write path, takes lock)
 * @param pmap Port map
 * @param key Five-tuple key
 * @param room_id Target room ID
 * @param member_id Target member ID
 * @return 0 on success, negative error code on failure
 */
int turbo_port_map_insert(struct turbo_port_map *pmap,
                          const struct turbo_five_tuple *key,
                          uint32_t room_id, uint32_t member_id);

/**
 * Lookup session mapping (read path, lock-free)
 * @param pmap Port map
 * @param key Five-tuple key
 * @param room_id Output room ID
 * @param member_id Output member ID
 * @return 0 on hit, -1 on miss
 */
int turbo_port_map_lookup(struct turbo_port_map *pmap,
                          const struct turbo_five_tuple *key,
                          uint32_t *room_id, uint32_t *member_id);

/**
 * Remove session mapping (write path, RCU-style mark+unlink)
 * @param pmap Port map
 * @param key Five-tuple key
 */
void turbo_port_map_remove(struct turbo_port_map *pmap,
                           const struct turbo_five_tuple *key);

/**
 * Reclaim deferred deletions via RCU.
 * Call periodically (e.g., from poll loop).
 * @param pmap Port map
 */
void turbo_port_map_reclaim(struct turbo_port_map *pmap);

/**
 * Hash function for five-tuple (supports both IPv4 and IPv6)
 * @param key Five-tuple key
 * @param seed Hash seed
 * @return Hash value
 */
static inline uint32_t turbo_five_tuple_hash(const struct turbo_five_tuple *key, uint32_t seed) {
    /* Jenkins one-at-a-time hash variant over entire struct */
    uint32_t hash = seed;
    const uint8_t *d = (const uint8_t *)key;
    size_t len = sizeof(struct turbo_five_tuple);
    for (size_t i = 0; i < len; ++i) {
        hash += d[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

#endif /* TURBO_PORT_H */
