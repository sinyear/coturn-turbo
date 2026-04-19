/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * https://opensource.org/license/bsd-3-clause
 *
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 */

#ifndef __CONDUCTOR_API_SERVER_H__
#define __CONDUCTOR_API_SERVER_H__

#include <stdint.h>
#include <netinet/in.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/listener.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ─────────────────────────────────────────────── */

#define CONDUCTOR_DEFAULT_HTTP_PORT  8080
#define CONDUCTOR_DEFAULT_WS_PORT    8081
#define CONDUCTOR_MAX_ROOMS          65536
#define CONDUCTOR_MAX_NODES          256
#define CONDUCTOR_MAX_MEMBERS        4096
#define CONDUCTOR_VNODES_PER_NODE    150   /* consistent hash virtual nodes */
#define CONDUCTOR_NODE_TIMEOUT_SEC   30    /* seconds before a node is considered dead */
#define CONDUCTOR_WS_PING_INTERVAL   15    /* WebSocket ping interval in seconds */

/* ── TurboNode info ─────────────────────────────────────────── */

typedef struct conductor_node {
    char            id[64];         /* unique node ID, e.g. "turbo-192.168.1.10" */
    char            ip[46];         /* IPv4 or IPv6 address string */
    uint16_t        port;           /* turbo API port on the node */
    uint16_t        admin_port;     /* admin API port on the node */
    uint64_t        last_seen;      /* monotonic timestamp of last heartbeat */
    uint32_t        load;           /* current room count on the node */
    uint32_t        capacity;       /* max rooms the node can handle */
    uint8_t         alive;          /* 1 = alive, 0 = dead */
    uint8_t         _pad[3];
} conductor_node_t;

/* ── Room info ─────────────────────────────────────────────── */

typedef struct conductor_room {
    uint32_t        room_id;
    char            node_id[64];    /* assigned TurboNode ID */
    char            node_ip[46];    /* assigned TurboNode IP */
    uint16_t        node_port;      /* assigned TurboNode port */
    uint32_t        member_count;
    uint64_t        created_at;     /* monotonic timestamp */
} conductor_room_t;

/* ── WebSocket client session ──────────────────────────────── */

typedef struct ws_client {
    struct evhttp_connection *evcon;
    evutil_socket_t           fd;
    struct event             *read_event;
    struct event             *ping_event;
    uint64_t                  connect_time;
    char                      session_id[64];
    uint8_t                   ws_state;     /* 0=handshake, 1=open, 2=closed */
    uint8_t                   frame_state;
    uint8_t                   mask_key[4];
    uint64_t                  payload_len;
    uint64_t                  payload_read;
    uint8_t                  *payload_buf;
    uint8_t                   masked;
} ws_client_t;

/* ── Room manager ──────────────────────────────────────────── */

typedef struct room_manager {
    /* Redis connection (may be NULL for standalone mode) */
    void                     *redis_handle;   /* redis_context_handle */

    /* Room table */
    conductor_room_t          rooms[CONDUCTOR_MAX_ROOMS];
    uint32_t                  room_count;

    /* Node table */
    conductor_node_t          nodes[CONDUCTOR_MAX_NODES];
    uint32_t                  node_count;

    /* Consistent hash ring: sorted array of (hash, node_index) */
    struct {
        uint32_t hash;
        uint32_t node_index;
    } hash_ring[CONDUCTOR_MAX_NODES * CONDUCTOR_VNODES_PER_NODE];
    uint32_t                  ring_size;
} room_manager_t;

/* ── Conductor global context ──────────────────────────────── */

typedef struct conductor_ctx {
    struct event_base       *evbase;
    room_manager_t           mgr;

    /* HTTP API server */
    struct evhttp           *http_server;
    uint16_t                 http_port;

    /* WebSocket server */
    evutil_socket_t          ws_listen_fd;
    struct event            *ws_accept_event;
    uint16_t                 ws_port;

    /* Redis connection string (NULL if not using Redis) */
    char                    *redis_url;

    /* Shutdown flag */
    int                      shutdown;
} conductor_ctx_t;

/* ── Public API ────────────────────────────────────────────── */

int  room_mgr_init(room_manager_t *mgr);
void room_mgr_cleanup(room_manager_t *mgr);

/* Room operations */
int  room_mgr_create_room(room_manager_t *mgr, uint32_t room_id);
int  room_mgr_destroy_room(room_manager_t *mgr, uint32_t room_id);
conductor_room_t* room_mgr_get_room(room_manager_t *mgr, uint32_t room_id);
int  room_mgr_get_room_list(room_manager_t *mgr, conductor_room_t **out_list, uint32_t *out_count);

/* Node operations */
int  room_mgr_register_node(room_manager_t *mgr, const conductor_node_t *node);
int  room_mgr_unregister_node(room_manager_t *mgr, const char *node_id);
int  room_mgr_node_heartbeat(room_manager_t *mgr, const char *node_id, uint32_t load);
conductor_node_t* room_mgr_find_node(room_manager_t *mgr, const char *node_id);
void room_mgr_check_node_timeouts(room_manager_t *mgr);

/* Consistent hash assignment */
int  room_mgr_assign_node(room_manager_t *mgr, uint32_t room_id,
                          conductor_node_t **out_node);

/* ── Internal helpers (used across .c files) ──────────────── */

conductor_ctx_t* ws_get_global_ctx(void);
void ws_dispatch_message(conductor_ctx_t *ctx, const char *text, size_t len,
                         ws_client_t *client);

/* ── API server ────────────────────────────────────────────── */

int  api_server_start(conductor_ctx_t *ctx);
void api_server_stop(conductor_ctx_t *ctx);

/* ── WebSocket server ──────────────────────────────────────── */

int  ws_server_start(conductor_ctx_t *ctx);
void ws_server_stop(conductor_ctx_t *ctx);

/* ── Redis sync ────────────────────────────────────────────── */

int  redis_sync_init(conductor_ctx_t *ctx, const char *redis_url);
void redis_sync_cleanup(conductor_ctx_t *ctx);
int  redis_sync_publish(conductor_ctx_t *ctx, const char *channel, const char *msg);
void redis_sync_room_created(conductor_ctx_t *ctx, uint32_t room_id, const char *node_id);
void redis_sync_room_destroyed(conductor_ctx_t *ctx, uint32_t room_id);
void redis_sync_node_registered(conductor_ctx_t *ctx, const conductor_node_t *node);

#ifdef __cplusplus
}
#endif

#endif /* __CONDUCTOR_API_SERVER_H__ */
