/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Lightweight room broadcast engine.
 * Design doc §7.3-7.5.
 *
 * - Rooms are identified by string room_id (≤63 chars).
 * - Members are identified by string member_id + alloc_id.
 * - Rooms are created lazily on first add_member and destroyed automatically
 *   when the last member leaves.
 * - Member list is protected by a per-room pthread_rwlock_t.
 * - The global room table is protected by g_rooms_lock.
 * - Max members per room is configurable (default: 50).
 */

#ifndef TURBO_ROOM_H
#define TURBO_ROOM_H

#include "../netif/turbo_netif.h"
#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Data structures                                                      */
/* ------------------------------------------------------------------ */

struct turbo_room_member {
    char                   member_id[64];
    uint32_t               alloc_id;         /* back-link for removal */
    struct sockaddr_in6    peer_addr;        /* where to forward media */
    struct turbo_room_member *next;
};

struct turbo_room {
    char                   room_id[64];
    uint32_t               member_count;
    struct turbo_room_member *members;       /* singly-linked list */
    pthread_rwlock_t        lock;
    struct turbo_room      *next;            /* intrusive list for hash bucket */
};

/* ------------------------------------------------------------------ */
/* Global init / deinit                                                 */
/* ------------------------------------------------------------------ */

/*
 * Call once at server startup.
 * max_members: per-room cap (0 → use default of 50).
 */
void turbo_room_init(uint32_t max_members);
void turbo_room_deinit(void);

/* ------------------------------------------------------------------ */
/* Room lifecycle                                                       */
/* ------------------------------------------------------------------ */

/*
 * Add member to room, creating the room if it does not exist.
 * alloc_id: coturn allocation id (used by remove to find the room later).
 * peer_addr: IP/port where media should be forwarded for this member.
 *
 * Returns:
 *   0  — success
 *  -1  — room member cap exceeded (turbo_room_member_overflow logged)
 *  -2  — duplicate member_id within the same room
 *  -3  — alloc error
 */
int turbo_room_add_member(const char *room_id,
                           const char *member_id,
                           uint32_t alloc_id,
                           const struct sockaddr_in6 *peer_addr);

/*
 * Remove a member.  If the room becomes empty it is destroyed.
 * Safe to call even if room_id / member_id no longer exist.
 */
void turbo_room_remove_member(const char *room_id, const char *member_id);

/*
 * Remove member by alloc_id (called from allocation destroy callback
 * when only alloc_id is known, not member_id).
 */
void turbo_room_remove_alloc(uint32_t alloc_id);

/* ------------------------------------------------------------------ */
/* Broadcast                                                            */
/* ------------------------------------------------------------------ */

/*
 * Clone pkt and send to every member except the one whose alloc_id
 * matches sender_alloc_id.
 *
 * Returns number of destinations forwarded to.
 */
int turbo_room_broadcast(struct turbo_netif *netif,
                          const char *room_id,
                          uint32_t sender_alloc_id,
                          struct rtp_packet *pkt);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                          */
/* ------------------------------------------------------------------ */

/* Count live members in a room.  Returns 0 if room not found. */
uint32_t turbo_room_member_count(const char *room_id);

/* Overflow counter (atomic, for Prometheus) */
extern _Atomic uint64_t g_turbo_room_member_overflow;

/* Active room count (incremented on lazy create, decremented on auto-destroy) */
extern _Atomic uint32_t g_turbo_active_rooms;

#endif /* TURBO_ROOM_H */
