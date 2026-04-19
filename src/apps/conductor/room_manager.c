/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * https://opensource.org/license/bsd-3-clause
 *
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 */

#include "api_server.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <event2/util.h>

#if !defined(TURN_NO_OPENSSL)
#include <openssl/sha.h>
#endif

/* ── Helpers ───────────────────────────────────────────────── */

static uint64_t now_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#if !defined(TURN_NO_OPENSSL)
/* SHA-256 based hash for consistent hashing ring */
static uint32_t consistent_hash(const char *key, size_t key_len) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)key, key_len, digest);
    return (uint32_t)digest[0] << 24 | (uint32_t)digest[1] << 16 |
           (uint32_t)digest[2] << 8  | (uint32_t)digest[3];
}
#else
/* Fallback: FNV-1a hash when OpenSSL is not available */
static uint32_t consistent_hash(const char *key, size_t key_len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < key_len; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619u;
    }
    return hash;
}
#endif

/* ── Comparison for qsort / bsearch on hash ring ───────────── */

typedef struct {
    uint32_t hash;
    uint32_t node_index;
} ring_entry_t;

static int ring_cmp(const void *a, const void *b) {
    const ring_entry_t *ea = (const ring_entry_t *)a;
    const ring_entry_t *eb = (const ring_entry_t *)b;
    if (ea->hash < eb->hash) return -1;
    if (ea->hash > eb->hash) return 1;
    return 0;
}

/* ── Rebuild the consistent hash ring from current nodes ──── */

static void rebuild_hash_ring(room_manager_t *mgr) {
    mgr->ring_size = 0;

    for (uint32_t ni = 0; ni < mgr->node_count; ni++) {
        if (!mgr->nodes[ni].alive) continue;

        for (uint32_t v = 0; v < CONDUCTOR_VNODES_PER_NODE; v++) {
            char vnode_key[128];
            snprintf(vnode_key, sizeof(vnode_key), "%s:vnode:%u",
                     mgr->nodes[ni].id, v);

            uint32_t h = consistent_hash(vnode_key, strlen(vnode_key));
            uint32_t idx = mgr->ring_size;
            mgr->hash_ring[idx].hash = h;
            mgr->hash_ring[idx].node_index = ni;
            mgr->ring_size++;
        }
    }

    if (mgr->ring_size > 1) {
        qsort(mgr->hash_ring, mgr->ring_size, sizeof(mgr->hash_ring[0]), ring_cmp);
    }
}

/* ── Room manager lifecycle ────────────────────────────────── */

int room_mgr_init(room_manager_t *mgr) {
    if (!mgr) return -1;

    memset(mgr, 0, sizeof(*mgr));
    mgr->redis_handle = NULL;
    mgr->room_count = 0;
    mgr->node_count = 0;
    mgr->ring_size = 0;

    return 0;
}

void room_mgr_cleanup(room_manager_t *mgr) {
    if (!mgr) return;
    memset(mgr, 0, sizeof(*mgr));
}

/* ── Room operations ───────────────────────────────────────── */

int room_mgr_create_room(room_manager_t *mgr, uint32_t room_id) {
    if (!mgr || room_id == 0) return -1;

    /* Check duplicate */
    for (uint32_t i = 0; i < mgr->room_count; i++) {
        if (mgr->rooms[i].room_id == room_id) {
            return -2;  /* already exists */
        }
    }

    if (mgr->room_count >= CONDUCTOR_MAX_ROOMS) {
        return -3;  /* table full */
    }

    /* Assign a node via consistent hash */
    conductor_node_t *node = NULL;
    int rc = room_mgr_assign_node(mgr, room_id, &node);
    if (rc != 0 || !node) {
        return -4;  /* no available node */
    }

    conductor_room_t *room = &mgr->rooms[mgr->room_count];
    room->room_id = room_id;
    strncpy(room->node_id, node->id, sizeof(room->node_id) - 1);
    strncpy(room->node_ip, node->ip, sizeof(room->node_ip) - 1);
    room->node_port = node->port;
    room->member_count = 0;
    room->created_at = now_mono_ms();

    mgr->room_count++;

    /* Update node load */
    node->load++;

    return 0;
}

int room_mgr_destroy_room(room_manager_t *mgr, uint32_t room_id) {
    if (!mgr) return -1;

    for (uint32_t i = 0; i < mgr->room_count; i++) {
        if (mgr->rooms[i].room_id == room_id) {
            /* Decrement node load */
            conductor_node_t *node = room_mgr_find_node(mgr, mgr->rooms[i].node_id);
            if (node && node->load > 0) {
                node->load--;
            }

            /* Compact the array */
            if (i < mgr->room_count - 1) {
                mgr->rooms[i] = mgr->rooms[mgr->room_count - 1];
            }
            memset(&mgr->rooms[mgr->room_count - 1], 0, sizeof(conductor_room_t));
            mgr->room_count--;
            return 0;
        }
    }

    return -1;  /* not found */
}

conductor_room_t* room_mgr_get_room(room_manager_t *mgr, uint32_t room_id) {
    if (!mgr) return NULL;

    for (uint32_t i = 0; i < mgr->room_count; i++) {
        if (mgr->rooms[i].room_id == room_id) {
            return &mgr->rooms[i];
        }
    }
    return NULL;
}

int room_mgr_get_room_list(room_manager_t *mgr, conductor_room_t **out_list, uint32_t *out_count) {
    if (!mgr || !out_list || !out_count) return -1;

    *out_list = mgr->rooms;
    *out_count = mgr->room_count;
    return 0;
}

/* ── Node operations ───────────────────────────────────────── */

int room_mgr_register_node(room_manager_t *mgr, const conductor_node_t *node) {
    if (!mgr || !node) return -1;

    /* Check duplicate */
    for (uint32_t i = 0; i < mgr->node_count; i++) {
        if (strcmp(mgr->nodes[i].id, node->id) == 0) {
            /* Update existing node */
            mgr->nodes[i] = *node;
            mgr->nodes[i].alive = 1;
            mgr->nodes[i].last_seen = now_mono_ms();
            rebuild_hash_ring(mgr);
            return 0;
        }
    }

    if (mgr->node_count >= CONDUCTOR_MAX_NODES) {
        return -2;  /* table full */
    }

    mgr->nodes[mgr->node_count] = *node;
    mgr->nodes[mgr->node_count].alive = 1;
    mgr->nodes[mgr->node_count].last_seen = now_mono_ms();
    mgr->node_count++;

    rebuild_hash_ring(mgr);
    return 0;
}

int room_mgr_unregister_node(room_manager_t *mgr, const char *node_id) {
    if (!mgr || !node_id) return -1;

    for (uint32_t i = 0; i < mgr->node_count; i++) {
        if (strcmp(mgr->nodes[i].id, node_id) == 0) {
            mgr->nodes[i].alive = 0;

            /* Compact */
            if (i < mgr->node_count - 1) {
                mgr->nodes[i] = mgr->nodes[mgr->node_count - 1];
            }
            memset(&mgr->nodes[mgr->node_count - 1], 0, sizeof(conductor_node_t));
            mgr->node_count--;

            rebuild_hash_ring(mgr);
            return 0;
        }
    }

    return -1;  /* not found */
}

int room_mgr_node_heartbeat(room_manager_t *mgr, const char *node_id, uint32_t load) {
    if (!mgr || !node_id) return -1;

    conductor_node_t *node = room_mgr_find_node(mgr, node_id);
    if (!node) return -2;

    node->last_seen = now_mono_ms();
    node->alive = 1;
    node->load = load;
    return 0;
}

conductor_node_t* room_mgr_find_node(room_manager_t *mgr, const char *node_id) {
    if (!mgr || !node_id) return NULL;

    for (uint32_t i = 0; i < mgr->node_count; i++) {
        if (strcmp(mgr->nodes[i].id, node_id) == 0) {
            return &mgr->nodes[i];
        }
    }
    return NULL;
}

void room_mgr_check_node_timeouts(room_manager_t *mgr) {
    if (!mgr) return;

    uint64_t now = now_mono_ms();
    uint64_t timeout_ms = (uint64_t)CONDUCTOR_NODE_TIMEOUT_SEC * 1000;
    int changed = 0;

    for (uint32_t i = 0; i < mgr->node_count; i++) {
        if (mgr->nodes[i].alive &&
            (now - mgr->nodes[i].last_seen) > timeout_ms) {
            mgr->nodes[i].alive = 0;
            changed = 1;
        }
    }

    if (changed) {
        rebuild_hash_ring(mgr);
    }
}

/* ── Consistent hash node assignment ───────────────────────── */

int room_mgr_assign_node(room_manager_t *mgr, uint32_t room_id,
                         conductor_node_t **out_node) {
    if (!mgr || !out_node) return -1;

    if (mgr->ring_size == 0) {
        return -1;  /* no nodes in ring */
    }

    /* Hash the room_id */
    char key[32];
    snprintf(key, sizeof(key), "room:%u", room_id);
    uint32_t h = consistent_hash(key, strlen(key));

    /* Binary search: find first ring entry with hash >= h */
    uint32_t lo = 0, hi = mgr->ring_size;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (mgr->hash_ring[mid].hash < h) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    /* Wrap around if past the end */
    if (lo >= mgr->ring_size) {
        lo = 0;
    }

    uint32_t node_idx = mgr->hash_ring[lo].node_index;
    if (node_idx >= mgr->node_count || !mgr->nodes[node_idx].alive) {
        return -1;
    }

    *out_node = &mgr->nodes[node_idx];
    return 0;
}
