/*
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "turbo_room.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#define HASH_BUCKETS_DEFAULT 1024

static uint32_t room_hash_fn(uint32_t room_id, uint32_t seed) {
    return (room_id * 2654435761UL) ^ seed;
}

static int turbo_room_mgr_init_custom(struct turbo_room_mgr *mgr,
                                      uint32_t max_rooms,
                                      uint32_t max_members_per_room) {
    (void)max_rooms; (void)max_members_per_room;
    mgr->room_hash.num_buckets = HASH_BUCKETS_DEFAULT;
    mgr->room_hash.hash_seed   = 0xdeadbeef;
    mgr->room_hash.buckets     = calloc(mgr->room_hash.num_buckets,
                                        sizeof(struct turbo_room *));
    return mgr->room_hash.buckets ? 0 : -ENOMEM;
}

static void turbo_room_mgr_cleanup_custom(struct turbo_room_mgr *mgr) {
    if (!mgr->room_hash.buckets)
        return;
    for (uint32_t i = 0; i < mgr->room_hash.num_buckets; i++) {
        struct turbo_room *room = mgr->room_hash.buckets[i];
        while (room) {
            struct turbo_room *next = room->next;
            struct turbo_member *m = room->members;
            while (m) {
                struct turbo_member *nm = m->next;
                free(m);
                m = nm;
            }
            free(room);
            room = next;
        }
    }
    free(mgr->room_hash.buckets);
    mgr->room_hash.buckets = NULL;
}

static int turbo_room_create_custom(struct turbo_room_mgr *mgr, uint32_t room_id) {
    uint32_t bucket = room_hash_fn(room_id, mgr->room_hash.hash_seed)
                      % mgr->room_hash.num_buckets;
    struct turbo_room *room = mgr->room_hash.buckets[bucket];
    while (room) {
        if (room->room_id == room_id && !room->marked_for_delete)
            return -EEXIST;
        room = room->next;
    }

    room = malloc(sizeof(*room));
    if (!room) return -ENOMEM;
    memset(room, 0, sizeof(*room));
    room->room_id = room_id;
    room->next = mgr->room_hash.buckets[bucket];
    mgr->room_hash.buckets[bucket] = room;
    return 0;
}

static void turbo_room_destroy_custom(struct turbo_room_mgr *mgr, uint32_t room_id) {
    uint32_t bucket = room_hash_fn(room_id, mgr->room_hash.hash_seed)
                      % mgr->room_hash.num_buckets;
    struct turbo_room *room = mgr->room_hash.buckets[bucket];
    struct turbo_room *prev = NULL;

    while (room) {
        if (room->room_id == room_id) {
            if (prev)
                prev->next = room->next;
            else
                mgr->room_hash.buckets[bucket] = room->next;
            struct turbo_member *m = room->members;
            while (m) {
                struct turbo_member *nm = m->next;
                free(m);
                m = nm;
            }
            free(room);
            return;
        }
        prev = room;
        room = room->next;
    }
}

static struct turbo_room *turbo_room_get_custom(struct turbo_room_mgr *mgr,
                                                uint32_t room_id) {
    uint32_t bucket = room_hash_fn(room_id, mgr->room_hash.hash_seed)
                      % mgr->room_hash.num_buckets;
    struct turbo_room *room = mgr->room_hash.buckets[bucket];
    while (room) {
        if (room->room_id == room_id && !room->marked_for_delete)
            return room;
        room = room->next;
    }
    return NULL;
}

/* Public API */

int turbo_room_mgr_init(struct turbo_room_mgr *mgr, struct turbo_netif *netif,
                        uint32_t max_rooms, uint32_t max_members_per_room) {
    if (!mgr || !netif) return -EINVAL;
    memset(mgr, 0, sizeof(*mgr));
    mgr->netif = netif;
    mgr->max_rooms = max_rooms;
    mgr->max_members_per_room = max_members_per_room;
    return turbo_room_mgr_init_custom(mgr, max_rooms, max_members_per_room);
}

struct turbo_room_mgr *turbo_room_mgr_create(struct turbo_netif *netif,
                                             uint32_t max_rooms,
                                             uint32_t max_members_per_room) {
    if (!netif) return NULL;
    struct turbo_room_mgr *mgr = malloc(sizeof(*mgr));
    if (!mgr) return NULL;
    if (turbo_room_mgr_init(mgr, netif, max_rooms, max_members_per_room) != 0) {
        free(mgr);
        return NULL;
    }
    return mgr;
}

void turbo_room_mgr_cleanup(struct turbo_room_mgr *mgr) {
    if (mgr) turbo_room_mgr_cleanup_custom(mgr);
}

void turbo_room_mgr_destroy(struct turbo_room_mgr *mgr) {
    if (!mgr) return;
    turbo_room_mgr_cleanup(mgr);
    free(mgr);
}

int turbo_room_create(struct turbo_room_mgr *mgr, uint32_t room_id) {
    if (!mgr) return -EINVAL;
    return turbo_room_create_custom(mgr, room_id);
}

void turbo_room_destroy(struct turbo_room_mgr *mgr, uint32_t room_id) {
    if (mgr) turbo_room_destroy_custom(mgr, room_id);
}

struct turbo_room *turbo_room_get(struct turbo_room_mgr *mgr, uint32_t room_id) {
    if (!mgr) return NULL;
    return turbo_room_get_custom(mgr, room_id);
}

int turbo_room_add_member(struct turbo_room_mgr *mgr, uint32_t room_id,
                          uint32_t member_id, struct sockaddr_in *addr, uint16_t port) {
    if (!mgr || !addr) return -EINVAL;
    struct turbo_room *room = turbo_room_get(mgr, room_id);
    if (!room) return -ENOENT;
    if (room->member_count >= mgr->max_members_per_room) return -ENOSPC;

    struct turbo_member *member = malloc(sizeof(*member));
    if (!member) return -ENOMEM;
    memset(member, 0, sizeof(*member));
    member->id   = member_id;
    member->addr = *addr;
    member->port = port;
    member->next = room->members;
    room->members = member;
    room->member_count++;
    return 0;
}

void turbo_room_remove_member(struct turbo_room_mgr *mgr, uint32_t room_id,
                              uint32_t member_id) {
    if (!mgr) return;
    struct turbo_room *room = turbo_room_get(mgr, room_id);
    if (!room) return;

    struct turbo_member *member = room->members;
    struct turbo_member *prev   = NULL;
    while (member) {
        if (member->id == member_id) {
            if (prev)
                prev->next = member->next;
            else
                room->members = member->next;
            room->member_count--;
            free(member);
            return;
        }
        prev   = member;
        member = member->next;
    }
}

/* Phase 4 placeholder — will be rewritten with RCU member list and rtp_packet API */
int turbo_room_broadcast(struct turbo_room_mgr *mgr, uint32_t room_id,
                         uint32_t sender_id, struct rtp_packet *pkt) {
    (void)mgr; (void)room_id; (void)sender_id; (void)pkt;
    return 0;
}

void turbo_room_mgr_reclaim(struct turbo_room_mgr *mgr) {
    (void)mgr; /* No deferred operations in non-DPDK path */
}
