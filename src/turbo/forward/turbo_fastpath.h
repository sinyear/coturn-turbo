/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Three-table fast-path lookup engine + L1 cache warmup.
 * Design doc §4.3, §6.2.
 *
 * Table hierarchy:
 *   channel_table : hash(src_ip, src_port, channel_no) → alloc_id
 *   l1_cache      : hash(src_ip, src_port)             → alloc_id
 *   username_table: username_hash                       → alloc_id
 */

#ifndef TURBO_FASTPATH_H
#define TURBO_FASTPATH_H

#include "../netif/turbo_netif.h"
#include "../common/turbo_seqlock.h"
#include <stdint.h>
#include <netinet/in.h>
#include <pthread.h>

/* Maximum allocation id (zero = invalid / miss) */
#define TURBO_ALLOC_ID_INVALID 0

/* Size of each hash table (power of two) */
#define TURBO_CHANNEL_TABLE_SIZE  4096
#define TURBO_L1_CACHE_SIZE       8192
#define TURBO_USERNAME_TABLE_SIZE 4096

/* ------------------------------------------------------------------ */
/* Snapshot read by worker thread (seqlock-protected)                  */
/* ------------------------------------------------------------------ */

struct turbo_alloc_snapshot {
    uint32_t             alloc_id;
    struct sockaddr_in6  peer_addr;   /* forwarding destination */
    char                 room_id[64]; /* empty string if no room */
    uint64_t             expiry;      /* UNIX seconds; 0 = no expiry */
};

/* ------------------------------------------------------------------ */
/* Individual table entries                                             */
/* ------------------------------------------------------------------ */

struct turbo_channel_entry {
    uint64_t key;        /* hash(src_ip, src_port, channel_no) */
    uint32_t alloc_id;
    uint8_t  used;
};

struct turbo_l1_entry {
    uint64_t key;        /* hash(src_ip, src_port) */
    uint32_t alloc_id;
    uint8_t  used;
};

struct turbo_username_entry {
    uint64_t key;        /* djb2 hash of username string */
    uint32_t alloc_id;
    uint8_t  used;
};

/* Alloc snapshot table entry */
struct turbo_alloc_entry {
    struct turbo_alloc_snapshot snap;
    struct turbo_seqlock        seqlock;
    uint8_t                     used;
};

/* ------------------------------------------------------------------ */
/* Main fastpath struct                                                 */
/* ------------------------------------------------------------------ */

#define TURBO_ALLOC_TABLE_SIZE 65536  /* must be >= max allocations */

struct turbo_fastpath {
    struct turbo_channel_entry  channel_table[TURBO_CHANNEL_TABLE_SIZE];
    struct turbo_l1_entry       l1_cache[TURBO_L1_CACHE_SIZE];
    struct turbo_username_entry username_table[TURBO_USERNAME_TABLE_SIZE];
    struct turbo_alloc_entry    alloc_table[TURBO_ALLOC_TABLE_SIZE];

    /* Protects all write paths (called rarely from libevent thread) */
    pthread_mutex_t write_lock;

    /* Statistics (atomic) */
    _Atomic uint64_t channel_hits;
    _Atomic uint64_t l1_hits;
    _Atomic uint64_t username_hits;
    _Atomic uint64_t misses;
};

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

int  turbo_fastpath_init(struct turbo_fastpath *fp);
void turbo_fastpath_deinit(struct turbo_fastpath *fp);

/*
 * Lookup function — called by turbo worker thread (lock-free read).
 * Returns alloc_id on hit, TURBO_ALLOC_ID_INVALID on miss.
 *
 * buf/len: raw UDP payload (starting at STUN/ChannelData header).
 * src: sender address.
 */
uint32_t turbo_fastpath_lookup(struct turbo_fastpath *fp,
                                const uint8_t *buf, size_t len,
                                const struct sockaddr_in6 *src);

/*
 * Read allocation snapshot via seqlock (for worker thread).
 * Returns 0 on success, -1 if alloc_id is not found or expired.
 */
int turbo_fastpath_read_alloc(struct turbo_fastpath *fp,
                               uint32_t alloc_id,
                               struct turbo_alloc_snapshot *snap);

/*
 * Warmup L1 cache — called by libevent thread after successful Allocate.
 * Also registers the allocation snapshot (peer addr, room_id, expiry).
 */
void turbo_fastpath_warmup(struct turbo_fastpath *fp,
                            uint32_t alloc_id,
                            const struct sockaddr_in6 *client_addr,
                            const struct sockaddr_in6 *peer_addr,
                            const char *room_id,
                            uint64_t expiry);

/*
 * Register a ChannelBind — called by libevent thread.
 */
void turbo_fastpath_add_channel(struct turbo_fastpath *fp,
                                 uint32_t alloc_id,
                                 const struct sockaddr_in6 *client_addr,
                                 uint16_t channel_no);

/*
 * Remove an allocation (called on allocation expiry/deletion).
 */
void turbo_fastpath_remove(struct turbo_fastpath *fp, uint32_t alloc_id);

#endif /* TURBO_FASTPATH_H */
