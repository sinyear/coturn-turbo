/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * https://opensource.org/license/bsd-3-clause
 *
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 */

#include "turbo_api.h"
#include "../../apps/relay/mainrelay.h"
#include "../../apps/common/ns_turn_utils.h"

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <json-c/json.h>
#include <string.h>
#include <stdatomic.h>


static struct evhttp *turbo_http_server = NULL;
static struct event_base *turbo_event_base = NULL;
static pthread_t turbo_api_thread;
static atomic_int turbo_api_running = 0;

/* Helper: send JSON response */
static void send_json_response(struct evhttp_request *req, int code, struct json_object *obj) {
    const char *json_str = json_object_to_json_string(obj);
    struct evbuffer *buf = evbuffer_new();
    if (buf) {
        evbuffer_add_printf(buf, "%s", json_str ? json_str : "{}");
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
        evhttp_send_reply(req, code, "OK", buf);
        evbuffer_free(buf);
    }
}

/* Phase 5 placeholder — room API endpoints will be rewritten with new room model */
static void turbo_api_room_not_impl(struct evhttp_request *req, void *arg) {
    UNUSED_ARG(arg);
    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", json_object_new_string("not_implemented"));
    json_object_object_add(resp, "error",  json_object_new_string("room API available in Phase 5"));
    send_json_response(req, 501, resp);
    json_object_put(resp);
}

/* API server thread */
static void* turbo_api_thread_func(void *arg) {
    uint16_t port = (uintptr_t)arg;

    turbo_event_base = event_base_new();
    if (!turbo_event_base) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: failed to create event base\n");
        return NULL;
    }

    turbo_http_server = evhttp_new(turbo_event_base);
    if (!turbo_http_server) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: failed to create http server\n");
        event_base_free(turbo_event_base);
        return NULL;
    }

    evhttp_set_cb(turbo_http_server, "/v1/room/create", turbo_api_room_not_impl, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/join",   turbo_api_room_not_impl, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/leave",  turbo_api_room_not_impl, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/info",   turbo_api_room_not_impl, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/list",   turbo_api_room_not_impl, NULL);

    if (evhttp_bind_socket(turbo_http_server, "0.0.0.0", port) != 0) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: failed to bind to port %u\n", port);
        evhttp_free(turbo_http_server);
        event_base_free(turbo_event_base);
        return NULL;
    }

    atomic_store(&turbo_api_running, 1);
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo api: server started on port %u\n", port);

    event_base_dispatch(turbo_event_base);

    atomic_store(&turbo_api_running, 0);
    evhttp_free(turbo_http_server);
    event_base_free(turbo_event_base);
    turbo_http_server = NULL;
    turbo_event_base = NULL;

    return NULL;
}

int turbo_api_start(uint16_t port) {
    if (port == 0) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo api: no port configured, API server not started\n");
        return 0;
    }

    if (pthread_create(&turbo_api_thread, NULL, turbo_api_thread_func, (void*)(uintptr_t)port) != 0) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo api: failed to create API thread\n");
        return -1;
    }

    pthread_detach(turbo_api_thread);
    return 0;
}

void turbo_api_stop(void) {
    if (atomic_load(&turbo_api_running) && turbo_event_base) {
        event_base_loopexit(turbo_event_base, NULL);
    }
}

