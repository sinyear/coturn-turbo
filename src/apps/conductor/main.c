/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * https://opensource.org/license/bsd-3-clause
 *
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 */

/**
 * conductor - Distributed scheduling service for coturn-turbo
 *
 * Usage:
 *   conductor [options]
 *
 * Options:
 *   -l, --listen <addr>      HTTP API listen address  (default: 0.0.0.0)
 *   -p, --port <port>        HTTP API port            (default: 8080)
 *   -w, --ws-port <port>     WebSocket port           (default: 8081)
 *   -r, --redis <url>        Redis URL                (optional)
 *   -n, --node-id <id>       This conductor's ID      (for HA)
 *   -h, --help               Show help
 */

#include "api_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <event2/event.h>

static volatile int g_shutdown = 0;
static conductor_ctx_t g_ctx = {0};

/* ── Signal handler ────────────────────────────────────────── */

static void signal_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd;
    (void)what;
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;
    ctx->shutdown = 1;
    g_shutdown = 1;
    event_base_loopexit(ctx->evbase, NULL);
}

/* ── Node timeout checker timer ────────────────────────────── */

static void node_timeout_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd;
    (void)what;
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;
    room_mgr_check_node_timeouts(&ctx->mgr);
}

/* ── Usage ─────────────────────────────────────────────────── */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  -l, --listen <addr>    HTTP API listen address (default: 0.0.0.0)\n"
        "  -p, --port <port>      HTTP API port           (default: 8080)\n"
        "  -w, --ws-port <port>   WebSocket port          (default: 8081)\n"
        "  -r, --redis <url>      Redis URL               (optional)\n"
        "  -n, --node-id <id>     This conductor's ID     (optional)\n"
        "  -h, --help             Show this help\n",
        prog);
}

/* ── Main ──────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *listen_addr = "0.0.0.0";
    uint16_t http_port = CONDUCTOR_DEFAULT_HTTP_PORT;
    uint16_t ws_port = CONDUCTOR_DEFAULT_WS_PORT;
    const char *redis_url = NULL;
    const char *node_id = "conductor-default";

    static struct option long_opts[] = {
        { "listen",  required_argument, NULL, 'l' },
        { "port",    required_argument, NULL, 'p' },
        { "ws-port", required_argument, NULL, 'w' },
        { "redis",   required_argument, NULL, 'r' },
        { "node-id", required_argument, NULL, 'n' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "l:p:w:r:n:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'l': listen_addr = optarg; break;
        case 'p': http_port = (uint16_t)atoi(optarg); break;
        case 'w': ws_port = (uint16_t)atoi(optarg); break;
        case 'r': redis_url = optarg; break;
        case 'n': node_id = optarg; break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Initialize global context */
    conductor_ctx_t *ctx = &g_ctx;
    memset(ctx, 0, sizeof(*ctx));

    ctx->http_port = http_port;
    ctx->ws_port = ws_port;
    ctx->shutdown = 0;

    if (redis_url) {
        ctx->redis_url = strdup(redis_url);
    }

    /* Initialize room manager */
    if (room_mgr_init(&ctx->mgr) != 0) {
        fprintf(stderr, "Failed to initialize room manager\n");
        return 1;
    }

    printf("Conductor initialized:\n");
    printf("  HTTP API:  %s:%u\n", listen_addr, http_port);
    printf("  WebSocket: %s:%u\n", listen_addr, ws_port);
    printf("  Redis:     %s\n", redis_url ? redis_url : "disabled");
    printf("  Node ID:   %s\n", node_id);

    /* Initialize libevent */
    ctx->evbase = event_base_new();
    if (!ctx->evbase) {
        fprintf(stderr, "Failed to create event base\n");
        return 1;
    }

    /* Signal handlers (SIGTERM, SIGINT) */
    struct event *sigterm = evsignal_new(ctx->evbase, SIGTERM, signal_cb, ctx);
    struct event *sigint = evsignal_new(ctx->evbase, SIGINT, signal_cb, ctx);
    if (sigterm) event_add(sigterm, NULL);
    if (sigint) event_add(sigint, NULL);

    /* Node timeout checker (runs every 10 seconds) */
    struct timeval timeout_tv = { 10, 0 };
    struct event *timeout_event = event_new(ctx->evbase, -1, EV_PERSIST,
                                             node_timeout_cb, ctx);
    if (timeout_event) {
        event_add(timeout_event, &timeout_tv);
    }

    /* Initialize Redis sync if configured */
    if (redis_url) {
        if (redis_sync_init(ctx, redis_url) != 0) {
            fprintf(stderr, "Warning: Redis connection failed, running without sync\n");
        }
    }

    /* Start HTTP API server */
    if (api_server_start(ctx) != 0) {
        fprintf(stderr, "Failed to start HTTP API server on port %u\n", http_port);
        return 1;
    }

    /* Start WebSocket server */
    if (ws_server_start(ctx) != 0) {
        fprintf(stderr, "Failed to start WebSocket server on port %u\n", ws_port);
        api_server_stop(ctx);
        return 1;
    }

    printf("Conductor service started successfully\n");
    fflush(stdout);

    /* Event loop */
    event_base_dispatch(ctx->evbase);

    /* Cleanup */
    printf("Shutting down conductor...\n");

    ws_server_stop(ctx);
    api_server_stop(ctx);

    if (timeout_event) {
        event_del(timeout_event);
        event_free(timeout_event);
    }

    if (sigterm) { event_del(sigterm); event_free(sigterm); }
    if (sigint)  { event_del(sigint);  event_free(sigint);  }

    redis_sync_cleanup(ctx);
    if (ctx->redis_url) { free(ctx->redis_url); }

    room_mgr_cleanup(&ctx->mgr);

    event_base_free(ctx->evbase);

    printf("Conductor stopped\n");
    return 0;
}
