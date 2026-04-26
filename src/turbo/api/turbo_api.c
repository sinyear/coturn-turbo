/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Admin HTTP API — Phase 5 implementation.
 * Design doc §11.1 (Prometheus metrics), §11.2 (JSON status).
 *
 * All handler callbacks run in the API thread's libevent loop.
 * Reads of shared globals are atomic-load or behind read locks
 * (no writes to shared state except for degrade/drain triggers).
 */

#include "turbo_api.h"
#include "../turbo.h"
#include "../netif/turbo_netif.h"
#include "../forward/turbo_fastpath.h"
#include "../forward/turbo_shaper.h"
#include "../room/turbo_room.h"
#include "../../apps/relay/mainrelay.h"
#include "../../apps/common/ns_turn_utils.h"

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Internal state                                                       */
/* ------------------------------------------------------------------ */

static struct evhttp   *g_api_http    = NULL;
static struct event_base *g_api_base  = NULL;
static pthread_t        g_api_thread;
static atomic_int       g_api_running = 0;

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static const char *backend_name(int bt) {
    switch (bt) {
    case TURBO_BACKEND_AF_XDP:   return "af_xdp";
    case TURBO_BACKEND_IO_URING: return "io_uring";
    case TURBO_BACKEND_EPOLL:    return "epoll";
    default:                     return "unknown";
    }
}

static const char *health_name(int hs) {
    switch (hs) {
    case TURBO_HEALTH_OK:       return "ok";
    case TURBO_HEALTH_DEGRADED: return "degraded";
    case TURBO_HEALTH_FAILED:   return "failed";
    default:                    return "unknown";
    }
}

static void send_text(struct evhttp_request *req, int code,
                      const char *ctype, const char *body, size_t len) {
    struct evbuffer *evb = evbuffer_new();
    if (!evb) { evhttp_send_reply(req, 500, "Internal Server Error", NULL); return; }
    evbuffer_add(evb, body, len);
    evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", ctype);
    evhttp_send_reply(req, code, "OK", evb);
    evbuffer_free(evb);
}

/* ------------------------------------------------------------------ */
/* GET /admin/status                                                    */
/* ------------------------------------------------------------------ */

static void cb_status(struct evhttp_request *req, void *arg) {
    (void)arg;
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
        return;
    }

    uint64_t ch    = atomic_load_explicit(&g_turbo_fastpath.channel_hits,  __ATOMIC_RELAXED);
    uint64_t l1    = atomic_load_explicit(&g_turbo_fastpath.l1_hits,       __ATOMIC_RELAXED);
    uint64_t un    = atomic_load_explicit(&g_turbo_fastpath.username_hits, __ATOMIC_RELAXED);
    uint64_t ms    = atomic_load_explicit(&g_turbo_fastpath.misses,        __ATOMIC_RELAXED);
    uint64_t total = ch + l1 + un + ms;
    double hit_rate = total > 0 ? (100.0 * (double)(ch + l1 + un) / (double)total) : 0.0;

    size_t   allocs   = (size_t)atomic_load(&global_allocation_count);
    uint32_t rooms    = (uint32_t)atomic_load_explicit(&g_turbo_active_rooms,   __ATOMIC_RELAXED);
    uint32_t members  = (uint32_t)atomic_load_explicit(&g_turbo_total_members,  __ATOMIC_RELAXED);
    uint64_t degraded = g_turbo_netif.degraded_count;
    int      drained  = (int)turn_params.drain_turn_server;
    uint64_t uptime   = (uint64_t)(time(NULL) - (g_turbo_start_time ? g_turbo_start_time : time(NULL)));

    const char *backend  = backend_name(g_turbo_netif.backend_type);
    const char *health   = health_name(g_turbo_netif.health_status);
    const char *provider = g_turbo_room_provider ? g_turbo_room_provider->name : "none";

    char buf[768];
    int  n = snprintf(buf, sizeof(buf),
        "{"
        "\"turbo_enabled\":true,"
        "\"backend\":\"%s\","
        "\"backend_health\":\"%s\","
        "\"allocations\":%zu,"
        "\"fastpath_hit_rate\":%.2f,"
        "\"l1_miss_total\":%llu,"
        "\"room_provider\":\"%s\","
        "\"rooms_active\":%u,"
        "\"total_room_members\":%u,"
        "\"degraded_count\":%llu,"
        "\"uptime_seconds\":%llu,"
        "\"drain_mode\":%s"
        "}",
        backend, health, allocs, hit_rate, (unsigned long long)ms,
        provider, rooms, members,
        (unsigned long long)degraded, (unsigned long long)uptime,
        drained ? "true" : "false");

    if (n > 0 && n < (int)sizeof(buf))
        send_text(req, 200, "application/json", buf, (size_t)n);
    else
        evhttp_send_reply(req, 500, "Internal Server Error", NULL);
}

/* ------------------------------------------------------------------ */
/* GET /admin/metrics  (Prometheus text format)                        */
/* ------------------------------------------------------------------ */

static void cb_metrics(struct evhttp_request *req, void *arg) {
    (void)arg;
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
        return;
    }

    uint64_t ch    = atomic_load_explicit(&g_turbo_fastpath.channel_hits,  __ATOMIC_RELAXED);
    uint64_t l1    = atomic_load_explicit(&g_turbo_fastpath.l1_hits,       __ATOMIC_RELAXED);
    uint64_t un    = atomic_load_explicit(&g_turbo_fastpath.username_hits, __ATOMIC_RELAXED);
    uint64_t ms    = atomic_load_explicit(&g_turbo_fastpath.misses,        __ATOMIC_RELAXED);
    uint64_t rx    = g_turbo_netif.rx_packets;
    uint64_t tx    = g_turbo_netif.tx_packets;
    uint64_t drop  = g_turbo_netif.dropped_packets;
    uint64_t degr  = g_turbo_netif.degraded_count;
    uint64_t ov    = atomic_load_explicit(&g_turbo_room_member_overflow, __ATOMIC_RELAXED);
    uint64_t stun_pass = atomic_load_explicit(&g_turbo_shaper.stun_passed_total,  __ATOMIC_RELAXED);
    uint64_t stun_drop = atomic_load_explicit(&g_turbo_shaper.stun_dropped_total, __ATOMIC_RELAXED);
    uint64_t media     = atomic_load_explicit(&g_turbo_shaper.media_passed_total, __ATOMIC_RELAXED);
    size_t   allocs    = (size_t)atomic_load(&global_allocation_count);
    uint32_t rooms     = (uint32_t)atomic_load_explicit(&g_turbo_active_rooms, __ATOMIC_RELAXED);
    int bt = g_turbo_netif.backend_type;

    struct evbuffer *evb = evbuffer_new();
    if (!evb) { evhttp_send_reply(req, 500, "Internal Server Error", NULL); return; }

    /* fastpath hits */
    evbuffer_add_printf(evb,
        "# HELP turbo_fastpath_hits_total Fastpath lookup hits by table\n"
        "# TYPE turbo_fastpath_hits_total counter\n"
        "turbo_fastpath_hits_total{type=\"channelbind\"} %llu\n"
        "turbo_fastpath_hits_total{type=\"l1\"} %llu\n"
        "turbo_fastpath_hits_total{type=\"username\"} %llu\n",
        (unsigned long long)ch, (unsigned long long)l1, (unsigned long long)un);

    /* fastpath misses */
    evbuffer_add_printf(evb,
        "# HELP turbo_fastpath_misses_total Fastpath lookup misses\n"
        "# TYPE turbo_fastpath_misses_total counter\n"
        "turbo_fastpath_misses_total %llu\n",
        (unsigned long long)ms);

    /* network I/O */
    evbuffer_add_printf(evb,
        "# HELP turbo_rx_packets_total Packets received by the turbo worker\n"
        "# TYPE turbo_rx_packets_total counter\n"
        "turbo_rx_packets_total %llu\n"
        "# HELP turbo_tx_packets_total Packets sent by the turbo worker\n"
        "# TYPE turbo_tx_packets_total counter\n"
        "turbo_tx_packets_total %llu\n"
        "# HELP turbo_dropped_packets_total Packets dropped (ring full / errors)\n"
        "# TYPE turbo_dropped_packets_total counter\n"
        "turbo_dropped_packets_total %llu\n",
        (unsigned long long)rx, (unsigned long long)tx, (unsigned long long)drop);

    /* backend info */
    evbuffer_add_printf(evb,
        "# HELP turbo_backend Active network backend (1 = af_xdp, 2 = io_uring, 3 = epoll)\n"
        "# TYPE turbo_backend gauge\n"
        "turbo_backend{type=\"%s\"} 1\n"
        "# HELP turbo_degraded_total Number of backend degradation events\n"
        "# TYPE turbo_degraded_total counter\n"
        "turbo_degraded_total %llu\n",
        backend_name(bt), (unsigned long long)degr);

    /* shaper / STUN rate limiter */
    evbuffer_add_printf(evb,
        "# HELP turbo_stun_passed_total STUN packets forwarded to libevent\n"
        "# TYPE turbo_stun_passed_total counter\n"
        "turbo_stun_passed_total %llu\n"
        "# HELP turbo_stun_overload_drops_total STUN packets dropped by rate limiter\n"
        "# TYPE turbo_stun_overload_drops_total counter\n"
        "turbo_stun_overload_drops_total %llu\n"
        "# HELP turbo_media_passed_total Media packets processed by fast path\n"
        "# TYPE turbo_media_passed_total counter\n"
        "turbo_media_passed_total %llu\n",
        (unsigned long long)stun_pass, (unsigned long long)stun_drop,
        (unsigned long long)media);

    /* rooms */
    evbuffer_add_printf(evb,
        "# HELP turbo_room_member_overflow_total Room member overflow rejections\n"
        "# TYPE turbo_room_member_overflow_total counter\n"
        "turbo_room_member_overflow_total %llu\n"
        "# HELP turbo_rooms_active Current number of active rooms\n"
        "# TYPE turbo_rooms_active gauge\n"
        "turbo_rooms_active %u\n",
        (unsigned long long)ov, rooms);

    /* allocations */
    evbuffer_add_printf(evb,
        "# HELP turbo_allocs_current Current active TURN allocations\n"
        "# TYPE turbo_allocs_current gauge\n"
        "turbo_allocs_current %zu\n",
        allocs);

    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", "text/plain; version=0.0.4");
    evhttp_send_reply(req, 200, "OK", evb);
    evbuffer_free(evb);
}

/* ------------------------------------------------------------------ */
/* POST /admin/turbo-disable                                            */
/* ------------------------------------------------------------------ */

static void cb_turbo_disable(struct evhttp_request *req, void *arg) {
    (void)arg;
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
        return;
    }

    int ret = turbo_netif_degrade(&g_turbo_netif, DEGRADE_MANUAL);
    const char *body;
    int code;
    if (ret == 0) {
        body = "{\"status\":\"ok\",\"message\":\"backend degraded to epoll\"}";
        code = 200;
    } else {
        body = "{\"status\":\"noop\",\"message\":\"already at epoll or degrade in progress\"}";
        code = 200;
    }
    send_text(req, code, "application/json", body, strlen(body));
}

/* ------------------------------------------------------------------ */
/* POST /admin/turbo-enable                                             */
/* ------------------------------------------------------------------ */

static void cb_turbo_enable(struct evhttp_request *req, void *arg) {
    (void)arg;
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
        return;
    }
    /* Reset the degrade-request flag; actual backend switch needs worker restart */
    atomic_store(&turbo_degrade_requested, 0);
    const char *body = "{\"status\":\"ok\","
                       "\"message\":\"degrade flag reset; restart worker to re-activate high-perf backend\"}";
    send_text(req, 200, "application/json", body, strlen(body));
}

/* ------------------------------------------------------------------ */
/* POST /admin/drain                                                    */
/* ------------------------------------------------------------------ */

static void cb_drain(struct evhttp_request *req, void *arg) {
    (void)arg;
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
        return;
    }
    enable_drain_mode();
    const char *body = "{\"status\":\"ok\",\"message\":\"drain mode enabled; no new allocations accepted\"}";
    send_text(req, 200, "application/json", body, strlen(body));
}

/* ------------------------------------------------------------------ */
/* GET /admin/drain/status                                              */
/* ------------------------------------------------------------------ */

static void cb_drain_status(struct evhttp_request *req, void *arg) {
    (void)arg;
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        evhttp_send_reply(req, 405, "Method Not Allowed", NULL);
        return;
    }

    size_t allocs  = (size_t)atomic_load(&global_allocation_count);
    int draining   = (int)turn_params.drain_turn_server;

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"draining\":%s,\"allocations_remaining\":%zu}",
        draining ? "true" : "false", allocs);

    if (n > 0 && n < (int)sizeof(buf))
        send_text(req, 200, "application/json", buf, (size_t)n);
    else
        evhttp_send_reply(req, 500, "Internal Server Error", NULL);
}

/* ------------------------------------------------------------------ */
/* API server thread                                                    */
/* ------------------------------------------------------------------ */

static void *api_thread_func(void *arg) {
    uint16_t port = (uint16_t)(uintptr_t)arg;

    g_api_base = event_base_new();
    if (!g_api_base) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: event_base_new failed\n");
        return NULL;
    }

    g_api_http = evhttp_new(g_api_base);
    if (!g_api_http) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: evhttp_new failed\n");
        event_base_free(g_api_base);
        g_api_base = NULL;
        return NULL;
    }

    evhttp_set_cb(g_api_http, "/admin/status",        cb_status,        NULL);
    evhttp_set_cb(g_api_http, "/admin/metrics",       cb_metrics,       NULL);
    evhttp_set_cb(g_api_http, "/admin/turbo-disable", cb_turbo_disable, NULL);
    evhttp_set_cb(g_api_http, "/admin/turbo-enable",  cb_turbo_enable,  NULL);
    evhttp_set_cb(g_api_http, "/admin/drain",         cb_drain,         NULL);
    evhttp_set_cb(g_api_http, "/admin/drain/status",  cb_drain_status,  NULL);

    if (evhttp_bind_socket(g_api_http, "0.0.0.0", port) != 0) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: bind to port %u failed\n", port);
        evhttp_free(g_api_http);
        event_base_free(g_api_base);
        g_api_http = NULL;
        g_api_base = NULL;
        return NULL;
    }

    atomic_store(&g_api_running, 1);
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo api: listening on port %u\n", port);

    event_base_dispatch(g_api_base);

    atomic_store(&g_api_running, 0);
    evhttp_free(g_api_http);
    event_base_free(g_api_base);
    g_api_http = NULL;
    g_api_base = NULL;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                     */
/* ------------------------------------------------------------------ */

int turbo_api_start(uint16_t port) {
    if (port == 0) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO,
                      "turbo api: no port configured, API server disabled\n");
        return 0;
    }
    if (pthread_create(&g_api_thread, NULL, api_thread_func,
                       (void *)(uintptr_t)port) != 0) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: pthread_create failed\n");
        return -1;
    }
    pthread_detach(g_api_thread);
    return 0;
}

void turbo_api_stop(void) {
    if (atomic_load(&g_api_running) && g_api_base)
        event_base_loopexit(g_api_base, NULL);
}
