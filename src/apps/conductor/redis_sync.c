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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <event2/buffer.h>

#if defined(HAVE_HIREDIS)
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>
#endif

/* ── Redis keys used for state sync ────────────────────────── */

#define REDIS_KEY_ROOM_MAP     "conductor:room_map"    /* hash: room_id -> node_id */
#define REDIS_KEY_NODE_STATE   "conductor:node_state"   /* hash: node_id -> JSON    */
#define REDIS_CHANNEL_ROOMS    "conductor:rooms"        /* pubsub channel           */
#define REDIS_CHANNEL_NODES    "conductor:nodes"        /* pubsub channel           */

/* ── Redis context wrapper ─────────────────────────────────── */

typedef struct redis_sync_ctx {
#if defined(HAVE_HIREDIS)
    redisAsyncContext *ac;
    struct event_base *evbase;
#endif
    char *host;
    int port;
} redis_sync_ctx_t;

/* ── Parse Redis URL ───────────────────────────────────────── */

static int parse_redis_url(const char *url, char **host, int *port) {
    if (!url || !host || !port) return -1;

    *host = NULL;
    *port = 6379;

    /* Expected format: redis://[host]:[port] */
    const char *prefix = "redis://";
    if (strncmp(url, prefix, 8) != 0) return -1;

    const char *host_start = url + 8;
    const char *colon = strchr(host_start, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - host_start);
        *host = malloc(host_len + 1);
        memcpy(*host, host_start, host_len);
        (*host)[host_len] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) *port = 6379;
    } else {
        *host = strdup(host_start);
    }

    return 0;
}

/* ── Redis sync initialization ─────────────────────────────── */

int redis_sync_init(conductor_ctx_t *ctx, const char *redis_url) {
    if (!ctx || !redis_url) return -1;

#if !defined(HAVE_HIREDIS)
    fprintf(stderr, "Redis sync requested but hiredis not compiled in\n");
    return -1;
#else
    char *host = NULL;
    int port = 6379;

    if (parse_redis_url(redis_url, &host, &port) != 0) {
        fprintf(stderr, "Invalid Redis URL: %s\n", redis_url);
        return -1;
    }

    redis_sync_ctx_t *rctx = calloc(1, sizeof(redis_sync_ctx_t));
    if (!rctx) {
        free(host);
        return -1;
    }

    rctx->host = host;
    rctx->port = port;
    rctx->evbase = ctx->evbase;

    /* Connect to Redis */
    rctx->ac = redisAsyncConnect(host, port);
    if (!rctx->ac || rctx->ac->err) {
        fprintf(stderr, "Redis connection error: %s\n",
                rctx->ac ? rctx->ac->errstr : "unknown");
        if (rctx->ac) redisAsyncFree(rctx->ac);
        free(rctx->host);
        free(rctx);
        return -1;
    }

    /* Attach to libevent */
    if (redisLibeventAttach(rctx->ac, ctx->evbase) != REDIS_OK) {
        fprintf(stderr, "Failed to attach Redis to libevent\n");
        redisAsyncFree(rctx->ac);
        free(rctx->host);
        free(rctx);
        return -1;
    }

    /* Store handle in room manager */
    ctx->mgr.redis_handle = (void *)rctx;

    /* Publish conductor online event */
    redisAsyncCommand(rctx->ac, NULL, NULL,
                      "PUBLISH %s \"{\\\"event\\\":\\\"conductor_online\\\"}\"",
                      REDIS_CHANNEL_NODES);

    fprintf(stdout, "Redis sync connected: %s:%d\n", host, port);
    return 0;
#endif
}

/* ── Redis sync cleanup ────────────────────────────────────── */

void redis_sync_cleanup(conductor_ctx_t *ctx) {
    if (!ctx) return;

#if defined(HAVE_HIREDIS)
    redis_sync_ctx_t *rctx = (redis_sync_ctx_t *)ctx->mgr.redis_handle;
    if (rctx) {
        if (rctx->ac) {
            redisAsyncDisconnect(rctx->ac);
            redisAsyncFree(rctx->ac);
        }
        free(rctx->host);
        free(rctx);
    }
#endif
    ctx->mgr.redis_handle = NULL;
}

/* ── Publish to Redis channel ──────────────────────────────── */

int redis_sync_publish(conductor_ctx_t *ctx, const char *channel, const char *msg) {
    if (!ctx || !channel || !msg) return -1;

#if defined(HAVE_HIREDIS)
    redis_sync_ctx_t *rctx = (redis_sync_ctx_t *)ctx->mgr.redis_handle;
    if (!rctx || !rctx->ac) return -1;

    return redisAsyncCommand(rctx->ac, NULL, NULL,
                             "PUBLISH %s %s", channel, msg);
#else
    (void)channel;
    (void)msg;
    return -1;
#endif
}

/* ── Helper: sync room creation to Redis ───────────────────── */

void redis_sync_room_created(conductor_ctx_t *ctx, uint32_t room_id,
                              const char *node_id) {
    if (!ctx || !ctx->mgr.redis_handle) return;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"event\":\"room_created\",\"room_id\":%u,\"node_id\":\"%s\"}",
             room_id, node_id);

    redis_sync_publish(ctx, REDIS_CHANNEL_ROOMS, msg);
}

/* ── Helper: sync room destruction to Redis ────────────────── */

void redis_sync_room_destroyed(conductor_ctx_t *ctx, uint32_t room_id) {
    if (!ctx || !ctx->mgr.redis_handle) return;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"event\":\"room_destroyed\",\"room_id\":%u}", room_id);

    redis_sync_publish(ctx, REDIS_CHANNEL_ROOMS, msg);
}

/* ── Helper: sync node registration to Redis ───────────────── */

void redis_sync_node_registered(conductor_ctx_t *ctx, const conductor_node_t *node) {
    if (!ctx || !ctx->mgr.redis_handle || !node) return;

    char msg[512];
    snprintf(msg, sizeof(msg),
             "{\"event\":\"node_registered\",\"id\":\"%s\",\"ip\":\"%s\","
             "\"port\":%u,\"admin_port\":%u,\"capacity\":%u}",
             node->id, node->ip, node->port, node->admin_port, node->capacity);

    redis_sync_publish(ctx, REDIS_CHANNEL_NODES, msg);
}
