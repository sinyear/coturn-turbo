#include "turbo_room.h"
#include "turbo_switch.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <arpa/inet.h>

#ifdef TURN_USE_DPDK
#include <rte_malloc.h>
#include <rte_hash_crc.h>
#include <rte_rcu_qsbr.h>
#include <rte_lcore.h>
#include <rte_version.h>

/* DPDK version compatibility: rte_gettid was removed in 22.11 */
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
#define turbo_rte_gettid() rte_lcore_id()
#else
#define turbo_rte_gettid() rte_gettid()
#endif

/* DPDK 22.11+: rte_rcu_qsbr_defer signature changed.
 * In newer versions, defer queues are used; for compatibility,
 * we fall back to direct free when defer queue is not available. */
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
#define turbo_rte_qsbr_defer(qsbr, free_fn, ptr) do { \
    free_fn(ptr); \
} while (0)
#define turbo_rte_qsbr_quiescent(qsbr, tid) rte_rcu_qsbr_quiescent(qsbr, tid)
/* DPDK 22.11+ removed rte_rcu_qsbr_create/free; use static init + rte_free */
#define TURBO_RCU_USE_STATIC_INIT 1
#else
#define turbo_rte_qsbr_defer(qsbr, free_fn, ptr) rte_rcu_qsbr_defer(qsbr, (void (*)(void*))(free_fn), ptr)
#define turbo_rte_qsbr_quiescent(qsbr, tid) rte_rcu_qsbr_quiescent(qsbr, tid)
#endif
#endif

/* Hash function for room IDs */
static uint32_t room_hash(uint32_t room_id, uint32_t seed) {
#ifdef TURN_USE_DPDK
    return rte_hash_crc(&room_id, sizeof(room_id), seed);
#else
    /* Simple hash function for non-DPDK version */
    return (room_id * 2654435761UL) ^ seed;
#endif
}

/* Hash function for member IDs within a room */
__attribute__((unused))
static uint32_t member_hash(uint32_t member_id, uint32_t seed) {
    return room_hash(member_id, seed);
}

#ifdef TURN_USE_DPDK

/* DPDK-based room manager implementation */

static int turbo_room_mgr_init_dpdk(struct turbo_room_mgr *mgr,
                                   uint32_t max_rooms, uint32_t max_members_per_room) {
    struct rte_hash_parameters hash_params = {0};
    char hash_name[32];
    int ret;

    (void)max_members_per_room;

#ifdef TURBO_RCU_USE_STATIC_INIT
    /* DPDK 22.11+: use static initializer + rte_rcu_qsbr_init */
    struct rte_rcu_qsbr *qsbr;
    qsbr = rte_zmalloc("room_rcu", sizeof(struct rte_rcu_qsbr), 0);
    if (!qsbr) {
        return -ENOMEM;
    }
    ret = rte_rcu_qsbr_init(qsbr, RTE_MAX_LCORE);
    if (ret < 0) {
        rte_free(qsbr);
        return -ENOMEM;
    }
    mgr->rcu = qsbr;
#else
    /* Older DPDK: use rte_rcu_qsbr_create */
    mgr->rcu = rte_rcu_qsbr_create("room_rcu", RTE_MAX_LCORE);
    if (!mgr->rcu) {
        return -ENOMEM;
    }
#endif

    mgr->rcu_thread_id = turbo_rte_gettid();

    /* Create hash table for rooms */
    snprintf(hash_name, sizeof(hash_name), "room_hash_%p", mgr);
    hash_params.name = hash_name;
    hash_params.entries = max_rooms;
    hash_params.key_len = sizeof(uint32_t);
    hash_params.hash_func = rte_hash_crc;
    hash_params.hash_func_init_val = 0;
    hash_params.socket_id = rte_socket_id();

    mgr->room_hash = rte_hash_create(&hash_params);
    if (!mgr->room_hash) {
#ifdef TURBO_RCU_USE_STATIC_INIT
        rte_free(mgr->rcu);
#else
        rte_rcu_qsbr_free(mgr->rcu, RTE_MAX_LCORE);
#endif
        return -ENOMEM;
    }

    return 0;
}

static void turbo_room_mgr_cleanup_dpdk(struct turbo_room_mgr *mgr) {
    if (mgr->room_hash) {
        rte_hash_free(mgr->room_hash);
    }
    if (mgr->rcu) {
#ifdef TURBO_RCU_USE_STATIC_INIT
        rte_free(mgr->rcu);
#else
        rte_rcu_qsbr_free(mgr->rcu, RTE_MAX_LCORE);
#endif
    }
}

static int turbo_room_create_dpdk(struct turbo_room_mgr *mgr, uint32_t room_id) {
    struct turbo_room *room;
    int ret;

    /* Check if room already exists */
    ret = rte_hash_lookup(mgr->room_hash, &room_id);
    if (ret >= 0) {
        return -EEXIST;
    }

    /* Allocate room */
    room = rte_malloc("turbo_room", sizeof(*room), 0);
    if (!room) {
        return -ENOMEM;
    }

    memset(room, 0, sizeof(*room));
    room->room_id = room_id;

    /* Add to hash table */
    ret = rte_hash_add_key_data(mgr->room_hash, &room_id, room);
    if (ret < 0) {
        rte_free(room);
        return -EIO;
    }

    return 0;
}

static void turbo_room_destroy_dpdk(struct turbo_room_mgr *mgr, uint32_t room_id) {
    struct turbo_room *room;
    int ret;

    /* Find room */
    ret = rte_hash_lookup_data(mgr->room_hash, &room_id, (void**)&room);
    if (ret < 0) {
        return;
    }

    /* Mark for deletion (RCU) */
    room->marked_for_delete = 1;

    /* Remove from hash table */
    rte_hash_del_key(mgr->room_hash, &room_id);

    /* Schedule for reclamation */
    turbo_rte_qsbr_defer(mgr->rcu, rte_free, room);
}

static struct turbo_room* turbo_room_get_dpdk(struct turbo_room_mgr *mgr, uint32_t room_id) {
    struct turbo_room *room;
    int ret;

    ret = rte_hash_lookup_data(mgr->room_hash, &room_id, (void**)&room);
    if (ret < 0) {
        return NULL;
    }

    /* Check if marked for deletion */
    if (room->marked_for_delete) {
        return NULL;
    }

    return room;
}

#else

/* Custom lock-free hash table implementation */

#define HASH_BUCKETS_DEFAULT 1024

static int turbo_room_mgr_init_custom(struct turbo_room_mgr *mgr,
                                    uint32_t max_rooms, uint32_t max_members_per_room) {
    (void)max_rooms; (void)max_members_per_room;
    mgr->room_hash.num_buckets = HASH_BUCKETS_DEFAULT;
    mgr->room_hash.hash_seed = 0xdeadbeef;
    mgr->room_hash.buckets = calloc(mgr->room_hash.num_buckets, sizeof(struct turbo_room*));
    if (!mgr->room_hash.buckets) {
        return -ENOMEM;
    }

    return 0;
}

static void turbo_room_mgr_cleanup_custom(struct turbo_room_mgr *mgr) {
    if (mgr->room_hash.buckets) {
        /* Free all rooms */
        for (uint32_t i = 0; i < mgr->room_hash.num_buckets; i++) {
            struct turbo_room *room = mgr->room_hash.buckets[i];
            while (room) {
                struct turbo_room *next = room->next;
                /* Free members */
                struct turbo_member *member = room->members;
                while (member) {
                    struct turbo_member *next_member = member->next;
                    free(member);
                    member = next_member;
                }
                free(room);
                room = next;
            }
        }
        free(mgr->room_hash.buckets);
    }
}

static int turbo_room_create_custom(struct turbo_room_mgr *mgr, uint32_t room_id) {
    uint32_t bucket = room_hash(room_id, mgr->room_hash.hash_seed) % mgr->room_hash.num_buckets;
    struct turbo_room *room;

    /* Check if room already exists */
    room = mgr->room_hash.buckets[bucket];
    while (room) {
        if (room->room_id == room_id && !room->marked_for_delete) {
            return -EEXIST;
        }
        room = room->next;
    }

    /* Allocate room */
    room = malloc(sizeof(*room));
    if (!room) {
        return -ENOMEM;
    }

    memset(room, 0, sizeof(*room));
    room->room_id = room_id;

    /* Add to bucket */
    room->next = mgr->room_hash.buckets[bucket];
    mgr->room_hash.buckets[bucket] = room;

    return 0;
}

static void turbo_room_destroy_custom(struct turbo_room_mgr *mgr, uint32_t room_id) {
    uint32_t bucket = room_hash(room_id, mgr->room_hash.hash_seed) % mgr->room_hash.num_buckets;
    struct turbo_room *room = mgr->room_hash.buckets[bucket];
    struct turbo_room *prev = NULL;

    while (room) {
        if (room->room_id == room_id) {
            /* Mark for deletion (RCU-style) */
            room->marked_for_delete = 1;
            /* Note: In a real RCU implementation, we'd defer the actual free */
            /* For simplicity, we'll free immediately here */
            if (prev) {
                prev->next = room->next;
            } else {
                mgr->room_hash.buckets[bucket] = room->next;
            }

            /* Free members */
            struct turbo_member *member = room->members;
            while (member) {
                struct turbo_member *next_member = member->next;
                free(member);
                member = next_member;
            }
            free(room);
            return;
        }
        prev = room;
        room = room->next;
    }
}

static struct turbo_room* turbo_room_get_custom(struct turbo_room_mgr *mgr, uint32_t room_id) {
    uint32_t bucket = room_hash(room_id, mgr->room_hash.hash_seed) % mgr->room_hash.num_buckets;
    struct turbo_room *room = mgr->room_hash.buckets[bucket];

    while (room) {
        if (room->room_id == room_id && !room->marked_for_delete) {
            return room;
        }
        room = room->next;
    }

    return NULL;
}

#endif

/* Public API implementation */

struct turbo_room_mgr* turbo_room_mgr_create(struct turbo_netif *netif,
    uint32_t max_rooms, uint32_t max_members_per_room) {
    struct turbo_room_mgr *mgr;

    if (!netif) {
        return NULL;
    }

#ifdef TURN_USE_DPDK
    mgr = rte_malloc("turbo_room_mgr", sizeof(*mgr), 0);
#else
    mgr = malloc(sizeof(*mgr));
#endif
    if (!mgr) {
        return NULL;
    }

    if (turbo_room_mgr_init(mgr, netif, max_rooms, max_members_per_room) != 0) {
#ifdef TURN_USE_DPDK
        rte_free(mgr);
#else
        free(mgr);
#endif
        return NULL;
    }

    return mgr;
}

void turbo_room_mgr_destroy(struct turbo_room_mgr *mgr) {
    if (!mgr) {
        return;
    }

    turbo_room_mgr_cleanup(mgr);

#ifdef TURN_USE_DPDK
    rte_free(mgr);
#else
    free(mgr);
#endif
}

int turbo_room_mgr_init(struct turbo_room_mgr *mgr, struct turbo_netif *netif,
                       uint32_t max_rooms, uint32_t max_members_per_room) {
    if (!mgr || !netif) {
        return -EINVAL;
    }

    memset(mgr, 0, sizeof(*mgr));
    mgr->netif = netif;
    mgr->max_rooms = max_rooms;
    mgr->max_members_per_room = max_members_per_room;

#ifdef TURN_USE_DPDK
    return turbo_room_mgr_init_dpdk(mgr, max_rooms, max_members_per_room);
#else
    return turbo_room_mgr_init_custom(mgr, max_rooms, max_members_per_room);
#endif
}

void turbo_room_mgr_cleanup(struct turbo_room_mgr *mgr) {
    if (!mgr) {
        return;
    }

#ifdef TURN_USE_DPDK
    turbo_room_mgr_cleanup_dpdk(mgr);
#else
    turbo_room_mgr_cleanup_custom(mgr);
#endif
}

int turbo_room_create(struct turbo_room_mgr *mgr, uint32_t room_id) {
    if (!mgr) {
        return -EINVAL;
    }

#ifdef TURN_USE_DPDK
    return turbo_room_create_dpdk(mgr, room_id);
#else
    return turbo_room_create_custom(mgr, room_id);
#endif
}

void turbo_room_destroy(struct turbo_room_mgr *mgr, uint32_t room_id) {
    if (!mgr) {
        return;
    }

#ifdef TURN_USE_DPDK
    turbo_room_destroy_dpdk(mgr, room_id);
#else
    turbo_room_destroy_custom(mgr, room_id);
#endif
}

struct turbo_room* turbo_room_get(struct turbo_room_mgr *mgr, uint32_t room_id) {
    if (!mgr) {
        return NULL;
    }

#ifdef TURN_USE_DPDK
    return turbo_room_get_dpdk(mgr, room_id);
#else
    return turbo_room_get_custom(mgr, room_id);
#endif
}

int turbo_room_add_member(struct turbo_room_mgr *mgr, uint32_t room_id,
                         uint32_t member_id, struct sockaddr_in *addr, uint16_t port) {
    struct turbo_room *room;
    struct turbo_member *member;

    if (!mgr || !addr) {
        return -EINVAL;
    }

    room = turbo_room_get(mgr, room_id);
    if (!room) {
        return -ENOENT;
    }

    /* Check member limit */
    if (room->member_count >= mgr->max_members_per_room) {
        return -ENOSPC;
    }

    /* Allocate member */
#ifdef TURN_USE_DPDK
    member = rte_malloc("turbo_member", sizeof(*member), 0);
#else
    member = malloc(sizeof(*member));
#endif
    if (!member) {
        return -ENOMEM;
    }

    memset(member, 0, sizeof(*member));
    member->id = member_id;
    member->addr = *addr;
    member->port = port;

    /* Add to room's member hash table */
    /* For simplicity, we'll use a simple linked list per room */
    /* In production, each room should have its own hash table */
    member->next = room->members;
    room->members = member;
    room->member_count++;

    return 0;
}

void turbo_room_remove_member(struct turbo_room_mgr *mgr, uint32_t room_id,
                             uint32_t member_id) {
    struct turbo_room *room;
    struct turbo_member *member, *prev;

    if (!mgr) {
        return;
    }

    room = turbo_room_get(mgr, room_id);
    if (!room) {
        return;
    }

    /* Find and remove member */
    member = room->members;
    prev = NULL;
    while (member) {
        if (member->id == member_id) {
            /* Mark for deletion (RCU-style) */
            member->marked_for_delete = 1;

            /* Remove from list */
            if (prev) {
                prev->next = member->next;
            } else {
                room->members = member->next;
            }
            room->member_count--;

            /* Schedule for reclamation */
#ifdef TURN_USE_DPDK
            turbo_rte_qsbr_defer(mgr->rcu, rte_free, member);
#else
            /* For simplicity, free immediately */
            free(member);
#endif
            return;
        }
        prev = member;
        member = member->next;
    }
}

int turbo_room_broadcast(struct turbo_room_mgr *mgr, uint32_t room_id,
                        uint32_t sender_id, struct turbo_packet *pkt) {
    struct turbo_room *room;
    struct turbo_member *member;
    int sent_count = 0;

    if (!mgr || !pkt) {
        return 0;
    }

    room = turbo_room_get(mgr, room_id);
    if (!room) {
        return 0;
    }

    /* Determine address family from packet */
    uint8_t af = TURBO_AF_INET; /* Default to IPv4 */
    if (pkt->data && pkt->len >= 1) {
        uint8_t ip_version = ((const uint8_t *)pkt->data)[0] >> 4;
        af = (ip_version == 6) ? TURBO_AF_INET6 : TURBO_AF_INET;
    }

    /* Count non-sender members first to allocate batch array */
    int num_dsts = 0;
    member = room->members;
    while (member) {
        if (member->id != sender_id && !member->marked_for_delete) {
            num_dsts++;
        }
        member = member->next;
    }

    if (num_dsts == 0) {
        return 0;
    }

    /* Collect destination addresses (flat array of TURBO_MAX_ADDR_LEN per entry) */
    uint8_t *dst_addrs = malloc(num_dsts * TURBO_MAX_ADDR_LEN);
    uint16_t *dst_ports = malloc(num_dsts * sizeof(uint16_t));
    if (!dst_addrs || !dst_ports) {
        free(dst_addrs);
        free(dst_ports);
        return 0;
    }

    int idx = 0;
    member = room->members;
    while (member && idx < num_dsts) {
        if (member->id != sender_id && !member->marked_for_delete) {
            uint8_t *addr = dst_addrs + (idx * TURBO_MAX_ADDR_LEN);
            memset(addr, 0, TURBO_MAX_ADDR_LEN);

            if (af == TURBO_AF_INET6) {
                /* For IPv6, we need sockaddr_in6.
                 * Current member uses sockaddr_in; for IPv6 support,
                 * the member should store sockaddr_storage.
                 * For now, copy what we have and fall back to IPv4. */
                memcpy(addr, &member->addr.sin_addr, 4);
            } else {
                memcpy(addr, &member->addr.sin_addr, 4);
            }
            dst_ports[idx] = member->addr.sin_port;
            idx++;
        }
        member = member->next;
    }

    /* Batch size for burst TX (cap to avoid large stack allocation) */
    const int MAX_BATCH = 64;
    struct turbo_packet **tx_array = malloc(MAX_BATCH * sizeof(struct turbo_packet *));
    if (!tx_array) {
        free(dst_addrs);
        free(dst_ports);
        return 0;
    }

    /* Process in batches */
    int offset = 0;
    while (offset < num_dsts) {
        int batch_size = (offset + MAX_BATCH > num_dsts) ? (num_dsts - offset) : MAX_BATCH;

        int prepared = turbo_switch_broadcast(mgr->netif, pkt,
                                              dst_addrs + (offset * TURBO_MAX_ADDR_LEN),
                                              dst_ports + offset,
                                              batch_size,
                                              af,
                                              tx_array, MAX_BATCH);

        if (prepared > 0) {
            uint16_t sent = mgr->netif->ops->tx_burst(mgr->netif, tx_array, prepared);
            sent_count += sent;

            /* Free any unsent cloned packets */
            for (int i = sent; i < prepared; i++) {
                mgr->netif->ops->free_pkt(mgr->netif, tx_array[i]);
            }
        }

        offset += batch_size;
    }

    free(tx_array);
    free(dst_addrs);
    free(dst_ports);

    return sent_count;
}

void turbo_room_mgr_reclaim(struct turbo_room_mgr *mgr) {
    if (!mgr) {
        return;
    }

#ifdef TURN_USE_DPDK
    /* Process RCU deferred operations.
     * Note: rte_rcu_qsbr_check requires a valid lcore mask; in single-threaded
     * reclaim we just quiescent and let defer callbacks execute directly
     * (DPDK 22.11+ macros already do direct free). */
    turbo_rte_qsbr_quiescent(mgr->rcu, mgr->rcu_thread_id);
#endif
}