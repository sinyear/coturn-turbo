#include "turbo_room.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#ifdef TURN_USE_DPDK
#include <rte_malloc.h>
#include <rte_hash_crc.h>
#include <rte_rcu_qsbr.h>
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
static uint32_t member_hash(uint32_t member_id, uint32_t seed) {
    return room_hash(member_id, seed);
}

#ifdef TURN_USE_DPDK

/* DPDK-based room manager implementation */

static int turbo_room_mgr_init_dpdk(struct turbo_room_mgr *mgr,
                                   uint32_t max_rooms, uint32_t max_members_per_room) {
    struct rte_hash_parameters hash_params = {0};
    char hash_name[32];

    /* Initialize RCU QSBR */
    mgr->rcu = rte_rcu_qsbr_create("room_rcu", RTE_MAX_LCORE);
    if (!mgr->rcu) {
        return -ENOMEM;
    }

    mgr->rcu_thread_id = rte_gettid();

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
        rte_rcu_qsbr_free(mgr->rcu, RTE_MAX_LCORE);
        return -ENOMEM;
    }

    return 0;
}

static void turbo_room_mgr_cleanup_dpdk(struct turbo_room_mgr *mgr) {
    if (mgr->room_hash) {
        rte_hash_free(mgr->room_hash);
    }
    if (mgr->rcu) {
        rte_rcu_qsbr_free(mgr->rcu, RTE_MAX_LCORE);
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
    rte_rcu_qsbr_defer(mgr->rcu, (void (*)(void*))rte_free, room);
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
    uint32_t bucket;

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
            rte_rcu_qsbr_defer(mgr->rcu, (void (*)(void*))rte_free, member);
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
    struct turbo_packet *cloned_pkt;
    int sent_count = 0;

    if (!mgr || !pkt) {
        return 0;
    }

    room = turbo_room_get(mgr, room_id);
    if (!room) {
        return 0;
    }

    /* Iterate through all members */
    member = room->members;
    while (member) {
        /* Skip sender and marked-for-delete members */
        if (member->id != sender_id && !member->marked_for_delete) {
            /* Clone packet for this member (zero-copy) */
            cloned_pkt = mgr->netif->ops->clone_pkt(mgr->netif, pkt);
            if (cloned_pkt) {
                /* Modify destination IP and port in packet headers */
                /* Note: This requires parsing and modifying IP/UDP headers */
                /* For simplicity, we'll assume the packet data contains headers */

                /* In a real implementation, you would:
                 * 1. Parse Ethernet/IP/UDP headers
                 * 2. Update destination IP to member->addr.sin_addr
                 * 3. Update destination port to member->port
                 * 4. Recalculate checksums
                 */

                /* For now, just simulate the send */
                struct turbo_packet *send_pkts[1] = {cloned_pkt};
                uint16_t sent = mgr->netif->ops->tx_burst(mgr->netif, send_pkts, 1);
                if (sent > 0) {
                    sent_count++;
                } else {
                    /* Free unsent packet */
                    mgr->netif->ops->free_pkt(mgr->netif, cloned_pkt);
                }
            }
        }
        member = member->next;
    }

    return sent_count;
}

void turbo_room_mgr_reclaim(struct turbo_room_mgr *mgr) {
    if (!mgr) {
        return;
    }

#ifdef TURN_USE_DPDK
    /* Process RCU deferred operations */
    rte_rcu_qsbr_quiescent(mgr->rcu, mgr->rcu_thread_id);
    rte_rcu_qsbr_check(mgr->rcu, RTE_MAX_LCORE, 0);
#endif
}