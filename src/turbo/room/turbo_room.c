/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Room broadcast engine implementation.
 * Design doc §7.3-7.5.
 */

#include "turbo_room.h"
#include "../forward/turbo_switch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Internal globals                                                     */
/* ------------------------------------------------------------------ */

/* Open-addressing hash table of rooms indexed by room_id string.
 * Protected by g_rooms_lock for all writes; readers must also hold it
 * (shared lock for broadcast, exclusive for mutations).               */
#define ROOM_BUCKETS 256

static struct turbo_room  *g_buckets[ROOM_BUCKETS];
static pthread_rwlock_t    g_rooms_lock = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t            g_max_members = 50;

_Atomic uint64_t g_turbo_room_member_overflow = 0;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static uint32_t room_bucket(const char *room_id) {
    uint32_t h = 5381;
    for (const uint8_t *p = (const uint8_t *)room_id; *p; p++)
        h = ((h << 5) + h) ^ *p;
    return h % ROOM_BUCKETS;
}

/* Caller must hold g_rooms_lock (at least shared) */
static struct turbo_room *find_room_locked(const char *room_id) {
    uint32_t b = room_bucket(room_id);
    struct turbo_room *r = g_buckets[b];
    while (r) {
        if (strncmp(r->room_id, room_id, 64) == 0) return r;
        r = r->next;
    }
    return NULL;
}

/* Destroy room and free all members.  Caller holds exclusive g_rooms_lock. */
static void destroy_room_locked(struct turbo_room *room) {
    uint32_t b = room_bucket(room->room_id);

    /* Unlink from bucket chain */
    struct turbo_room **pp = &g_buckets[b];
    while (*pp && *pp != room) pp = &(*pp)->next;
    if (*pp) *pp = room->next;

    /* Free members */
    struct turbo_room_member *m = room->members;
    while (m) {
        struct turbo_room_member *nm = m->next;
        free(m);
        m = nm;
    }
    pthread_rwlock_destroy(&room->lock);
    free(room);
}

/* ------------------------------------------------------------------ */
/* Global init / deinit                                                 */
/* ------------------------------------------------------------------ */

void turbo_room_init(uint32_t max_members) {
    memset(g_buckets, 0, sizeof(g_buckets));
    g_max_members = (max_members > 0) ? max_members : 50;
}

void turbo_room_deinit(void) {
    pthread_rwlock_wrlock(&g_rooms_lock);
    for (int i = 0; i < ROOM_BUCKETS; i++) {
        struct turbo_room *r = g_buckets[i];
        while (r) {
            struct turbo_room *next = r->next;
            struct turbo_room_member *m = r->members;
            while (m) {
                struct turbo_room_member *nm = m->next;
                free(m);
                m = nm;
            }
            pthread_rwlock_destroy(&r->lock);
            free(r);
            r = next;
        }
        g_buckets[i] = NULL;
    }
    pthread_rwlock_unlock(&g_rooms_lock);
}

/* ------------------------------------------------------------------ */
/* Add member                                                           */
/* ------------------------------------------------------------------ */

int turbo_room_add_member(const char *room_id,
                           const char *member_id,
                           uint32_t alloc_id,
                           const struct sockaddr_in6 *peer_addr) {
    if (!room_id || !member_id || !peer_addr) return -3;

    pthread_rwlock_wrlock(&g_rooms_lock);

    struct turbo_room *room = find_room_locked(room_id);

    /* Lazy creation */
    if (!room) {
        room = calloc(1, sizeof(*room));
        if (!room) {
            pthread_rwlock_unlock(&g_rooms_lock);
            return -3;
        }
        strncpy(room->room_id, room_id, 63);
        pthread_rwlock_init(&room->lock, NULL);

        uint32_t b = room_bucket(room_id);
        room->next = g_buckets[b];
        g_buckets[b] = room;
    }

    /* Check member cap */
    if (room->member_count >= g_max_members) {
        atomic_fetch_add_explicit(&g_turbo_room_member_overflow, 1,
                                   __ATOMIC_RELAXED);
        pthread_rwlock_unlock(&g_rooms_lock);
        return -1;
    }

    /* Check for duplicate */
    struct turbo_room_member *m = room->members;
    while (m) {
        if (strncmp(m->member_id, member_id, 64) == 0) {
            pthread_rwlock_unlock(&g_rooms_lock);
            return -2;
        }
        m = m->next;
    }

    /* Insert new member */
    struct turbo_room_member *nm = calloc(1, sizeof(*nm));
    if (!nm) {
        pthread_rwlock_unlock(&g_rooms_lock);
        return -3;
    }
    strncpy(nm->member_id, member_id, 63);
    nm->alloc_id = alloc_id;
    nm->peer_addr = *peer_addr;
    nm->next = room->members;
    room->members = nm;
    room->member_count++;

    pthread_rwlock_unlock(&g_rooms_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Remove member by member_id                                           */
/* ------------------------------------------------------------------ */

void turbo_room_remove_member(const char *room_id, const char *member_id) {
    if (!room_id || !member_id) return;

    pthread_rwlock_wrlock(&g_rooms_lock);

    struct turbo_room *room = find_room_locked(room_id);
    if (!room) {
        pthread_rwlock_unlock(&g_rooms_lock);
        return;
    }

    struct turbo_room_member **pp = &room->members;
    while (*pp) {
        if (strncmp((*pp)->member_id, member_id, 64) == 0) {
            struct turbo_room_member *del = *pp;
            *pp = del->next;
            room->member_count--;
            free(del);
            break;
        }
        pp = &(*pp)->next;
    }

    /* Auto-destroy empty room */
    if (room->member_count == 0)
        destroy_room_locked(room);

    pthread_rwlock_unlock(&g_rooms_lock);
}

/* ------------------------------------------------------------------ */
/* Remove by alloc_id (ghost-member cleanup)                            */
/* ------------------------------------------------------------------ */

void turbo_room_remove_alloc(uint32_t alloc_id) {
    if (alloc_id == 0) return;

    pthread_rwlock_wrlock(&g_rooms_lock);

    for (int i = 0; i < ROOM_BUCKETS; i++) {
        struct turbo_room *r = g_buckets[i];
        while (r) {
            struct turbo_room *next_r = r->next;
            struct turbo_room_member **pp = &r->members;
            while (*pp) {
                if ((*pp)->alloc_id == alloc_id) {
                    struct turbo_room_member *del = *pp;
                    *pp = del->next;
                    r->member_count--;
                    free(del);
                    /* keep scanning — alloc_id should be unique, but be safe */
                    continue;
                }
                pp = &(*pp)->next;
            }
            if (r->member_count == 0)
                destroy_room_locked(r);
            r = next_r;
        }
    }

    pthread_rwlock_unlock(&g_rooms_lock);
}

/* ------------------------------------------------------------------ */
/* Broadcast                                                            */
/* ------------------------------------------------------------------ */

int turbo_room_broadcast(struct turbo_netif *netif,
                          const char *room_id,
                          uint32_t sender_alloc_id,
                          struct rtp_packet *pkt) {
    if (!netif || !room_id || !pkt) return 0;

    /* Build a destination list under shared lock to minimise critical section */
#define MAX_BROADCAST_DSTS 64
    struct sockaddr_in6 dsts[MAX_BROADCAST_DSTS];
    int ndsts = 0;

    pthread_rwlock_rdlock(&g_rooms_lock);

    struct turbo_room *room = find_room_locked(room_id);
    if (!room) {
        pthread_rwlock_unlock(&g_rooms_lock);
        return 0;
    }

    struct turbo_room_member *m = room->members;
    while (m && ndsts < MAX_BROADCAST_DSTS) {
        if (m->alloc_id != sender_alloc_id)
            dsts[ndsts++] = m->peer_addr;
        m = m->next;
    }

    pthread_rwlock_unlock(&g_rooms_lock);

    /* Send outside the lock */
    struct rtp_packet *tx[MAX_BROADCAST_DSTS];
    int n = turbo_switch_broadcast(netif, pkt, dsts, ndsts, tx, MAX_BROADCAST_DSTS);
    for (int i = 0; i < n; i++) {
        netif->ops->send_burst(netif, &tx[i], 1);
        netif->ops->free_pkt(netif, tx[i]);
    }

    return n;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                          */
/* ------------------------------------------------------------------ */

uint32_t turbo_room_member_count(const char *room_id) {
    if (!room_id) return 0;
    pthread_rwlock_rdlock(&g_rooms_lock);
    struct turbo_room *r = find_room_locked(room_id);
    uint32_t cnt = r ? r->member_count : 0;
    pthread_rwlock_unlock(&g_rooms_lock);
    return cnt;
}
