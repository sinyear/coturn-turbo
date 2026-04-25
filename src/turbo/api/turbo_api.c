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

#if defined(TURBO_FEATURES)
#include "../forward/turbo_room.h"
#endif

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>
#include <json-c/json.h>
#include <string.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include "../common/turbo_json.h"


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

/* Helper: parse JSON body from request */
static struct json_object* parse_json_body(struct evhttp_request *req) {
    struct evbuffer *input = evhttp_request_get_input_buffer(req);
    if (!input) return NULL;

    size_t len = evbuffer_get_length(input);
    if (len == 0 || len > 65536) return NULL;

    char *body = malloc(len + 1);
    if (!body) return NULL;

    evbuffer_copyout(input, body, len);
    body[len] = '\0';

    struct json_object *obj = json_tokener_parse(body);
    free(body);
    return obj;
}

/* POST /v1/room/create */
static void turbo_api_room_create(struct evhttp_request *req, void *arg) {
    UNUSED_ARG(arg);

    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json_response(req, 405, NULL);
        return;
    }

    struct json_object *body = parse_json_body(req);
    if (!body) {
        send_json_response(req, 400, NULL);
        return;
    }

    struct json_object *jroom_id = json_object_object_get(body, "room_id");
    if (!jroom_id || !json_object_is_type(jroom_id, json_type_int)) {
        json_object_put(body);
        send_json_response(req, 400, NULL);
        return;
    }

    uint32_t room_id = (uint32_t)json_object_get_int(jroom_id);
    int ret = turbo_room_create(turbo_room_mgr, room_id);

    struct json_object *resp = json_object_new_object();
    if (ret == 0 || ret == -17) { /* 0 = created, -17 = already exists */
        json_object_object_add(resp, "status", json_object_new_string("ok"));
        json_object_object_add(resp, "room_id", json_object_new_int(room_id));
        send_json_response(req, 200, resp);
    } else {
        json_object_object_add(resp, "status", json_object_new_string("error"));
        json_object_object_add(resp, "error", json_object_new_string("failed to create room"));
        send_json_response(req, 500, resp);
    }

    json_object_put(body);
    json_object_put(resp);
}

/* POST /v1/room/join */
static void turbo_api_room_join(struct evhttp_request *req, void *arg) {
    UNUSED_ARG(arg);

    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json_response(req, 405, NULL);
        return;
    }

    struct json_object *body = parse_json_body(req);
    if (!body) {
        send_json_response(req, 400, NULL);
        return;
    }

    struct json_object *jroom_id = json_object_object_get(body, "room_id");
    struct json_object *jmember_id = json_object_object_get(body, "member_id");

    if (!jroom_id || !jmember_id) {
        json_object_put(body);
        send_json_response(req, 400, NULL);
        return;
    }

    uint32_t room_id = (uint32_t)json_object_get_int(jroom_id);
    uint32_t member_id = (uint32_t)json_object_get_int(jmember_id);

    struct sockaddr_in addr;
    if (turbo_json_parse_addr(body, &addr) != 0) {
        json_object_put(body);
        send_json_response(req, 400, NULL);
        return;
    }
    uint16_t port = ntohs(addr.sin_port);

    int ret = turbo_room_add_member(turbo_room_mgr, room_id, member_id, &addr, port);

    struct json_object *resp = json_object_new_object();
    if (ret == 0) {
        json_object_object_add(resp, "status", json_object_new_string("ok"));
        json_object_object_add(resp, "room_id", json_object_new_int(room_id));
        json_object_object_add(resp, "member_id", json_object_new_int(member_id));
        send_json_response(req, 200, resp);
    } else {
        json_object_object_add(resp, "status", json_object_new_string("error"));
        if (ret == -2) {
            json_object_object_add(resp, "error", json_object_new_string("room not found"));
        } else if (ret == -28) {
            json_object_object_add(resp, "error", json_object_new_string("room full"));
        } else {
            json_object_object_add(resp, "error", json_object_new_string("failed to join room"));
        }
        send_json_response(req, 500, resp);
    }

    json_object_put(body);
    json_object_put(resp);
}

/* DELETE /v1/room/leave */
static void turbo_api_room_leave(struct evhttp_request *req, void *arg) {
    UNUSED_ARG(arg);

    if (evhttp_request_get_command(req) != EVHTTP_REQ_DELETE &&
        evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json_response(req, 405, NULL);
        return;
    }

    struct json_object *body = parse_json_body(req);
    if (!body) {
        send_json_response(req, 400, NULL);
        return;
    }

    struct json_object *jroom_id = json_object_object_get(body, "room_id");
    struct json_object *jmember_id = json_object_object_get(body, "member_id");

    if (!jroom_id || !jmember_id) {
        json_object_put(body);
        send_json_response(req, 400, NULL);
        return;
    }

    uint32_t room_id = (uint32_t)json_object_get_int(jroom_id);
    uint32_t member_id = (uint32_t)json_object_get_int(jmember_id);

    turbo_room_remove_member(turbo_room_mgr, room_id, member_id);

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", json_object_new_string("ok"));
    json_object_object_add(resp, "room_id", json_object_new_int(room_id));
    json_object_object_add(resp, "member_id", json_object_new_int(member_id));
    send_json_response(req, 200, resp);

    json_object_put(body);
    json_object_put(resp);
}

/* GET /v1/room/info */
static void turbo_api_room_info(struct evhttp_request *req, void *arg) {
    UNUSED_ARG(arg);

    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        send_json_response(req, 405, NULL);
        return;
    }

    const char *uri = evhttp_request_get_uri(req);
    struct evkeyvalq params;
    evhttp_parse_query(uri, &params);

    const char *room_id_str = evhttp_find_header(&params, "room_id");
    if (!room_id_str) {
        evhttp_clear_headers(&params);
        send_json_response(req, 400, NULL);
        return;
    }

    uint32_t room_id = (uint32_t)atoi(room_id_str);
    struct turbo_room *room = turbo_room_get(turbo_room_mgr, room_id);

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "room_id", json_object_new_int(room_id));

    if (room) {
        json_object_object_add(resp, "status", json_object_new_string("ok"));
        json_object_object_add(resp, "member_count", json_object_new_int(room->member_count));

        struct json_object *members = json_object_new_array();
        struct turbo_member *member = room->members;
        while (member) {
            if (!member->marked_for_delete) {
                struct json_object *m = json_object_new_object();
                json_object_object_add(m, "id", json_object_new_int(member->id));
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &member->addr.sin_addr, ip, sizeof(ip));
                json_object_object_add(m, "ip", json_object_new_string(ip));
                json_object_object_add(m, "port", json_object_new_int(member->port));
                json_object_array_add(members, m);
            }
            member = member->next;
        }
        json_object_object_add(resp, "members", members);
    } else {
        json_object_object_add(resp, "status", json_object_new_string("not_found"));
    }

    evhttp_clear_headers(&params);
    send_json_response(req, 200, resp);
    json_object_put(resp);
}

/* GET /v1/room/list */
static void turbo_api_room_list(struct evhttp_request *req, void *arg) {
    UNUSED_ARG(arg);

    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        send_json_response(req, 405, NULL);
        return;
    }

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", json_object_new_string("ok"));

    struct json_object *rooms = json_object_new_array();

    /* Iterate through rooms */
    for (uint32_t i = 0; i < turbo_room_mgr->room_hash.num_buckets; i++) {
        struct turbo_room *room = turbo_room_mgr->room_hash.buckets[i];
        while (room) {
            if (!room->marked_for_delete) {
                struct json_object *r = json_object_new_object();
                json_object_object_add(r, "room_id", json_object_new_int(room->room_id));
                json_object_object_add(r, "member_count", json_object_new_int(room->member_count));
                json_object_array_add(rooms, r);
            }
            room = room->next;
        }
    }

    json_object_object_add(resp, "rooms", rooms);
    json_object_object_add(resp, "total", json_object_new_int(json_object_array_length(rooms)));
    send_json_response(req, 200, resp);
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

    evhttp_set_cb(turbo_http_server, "/v1/room/create", turbo_api_room_create, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/join", turbo_api_room_join, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/leave", turbo_api_room_leave, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/info", turbo_api_room_info, NULL);
    evhttp_set_cb(turbo_http_server, "/v1/room/list", turbo_api_room_list, NULL);

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

