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
#include <json-c/json.h>
#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

/* ── Forward declarations of handler functions ─────────────── */

static void api_handle_node_register(struct evhttp_request *req, void *arg);
static void api_handle_node_unregister(struct evhttp_request *req, void *arg);
static void api_handle_node_heartbeat(struct evhttp_request *req, void *arg);
static void api_handle_node_list(struct evhttp_request *req, void *arg);
static void api_handle_room_create(struct evhttp_request *req, void *arg);
static void api_handle_room_destroy(struct evhttp_request *req, void *arg);
static void api_handle_room_info(struct evhttp_request *req, void *arg);
static void api_handle_room_list(struct evhttp_request *req, void *arg);
static void api_handle_room_assign(struct evhttp_request *req, void *arg);
static void api_handle_health(struct evhttp_request *req, void *arg);

/* ── Helpers ───────────────────────────────────────────────── */

static void send_json(struct evhttp_request *req, int code, struct json_object *obj) {
    const char *body = json_object_to_json_string(obj);
    struct evbuffer *buf = evbuffer_new();
    if (buf) {
        evbuffer_add_printf(buf, "%s", body ? body : "{}");
        evhttp_add_header(evhttp_request_get_output_headers(req),
                          "Content-Type", "application/json");
        evhttp_add_header(evhttp_request_get_output_headers(req),
                          "Access-Control-Allow-Origin", "*");
        evhttp_send_reply(req, code, "OK", buf);
        evbuffer_free(buf);
    }
}

static struct json_object* parse_request_body(struct evhttp_request *req) {
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

static const char* get_query_param(struct evhttp_request *req, const char *name) {
    const char *uri = evhttp_request_get_uri(req);
    struct evkeyvalq params;
    evhttp_parse_query(uri, &params);
    const char *val = evhttp_find_header(&params, name);
    evhttp_clear_headers(&params);
    return val;
}

/* ── Node handlers ─────────────────────────────────────────── */

/* POST /v1/node/register
 * Body: { "id": "turbo-1", "ip": "192.168.1.10", "port": 3478, "admin_port": 8080, "capacity": 1000 }
 */
static void api_handle_node_register(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;
    struct json_object *body = parse_request_body(req);
    if (!body) { send_json(req, 400, NULL); return; }

    struct json_object *jid = json_object_object_get(body, "id");
    struct json_object *jip = json_object_object_get(body, "ip");
    struct json_object *jport = json_object_object_get(body, "port");
    struct json_object *jadmin = json_object_object_get(body, "admin_port");
    struct json_object *jcap = json_object_object_get(body, "capacity");

    if (!jid || !jip || !jport) {
        json_object_put(body);
        send_json(req, 400, NULL);
        return;
    }

    conductor_node_t node = {0};
    strncpy(node.id, json_object_get_string(jid), sizeof(node.id) - 1);
    strncpy(node.ip, json_object_get_string(jip), sizeof(node.ip) - 1);
    node.port = (uint16_t)json_object_get_int(jport);
    node.admin_port = jadmin ? (uint16_t)json_object_get_int(jadmin) : 0;
    node.capacity = jcap ? (uint32_t)json_object_get_int(jcap) : 1000;

    int rc = room_mgr_register_node(&ctx->mgr, &node);

    struct json_object *resp = json_object_new_object();
    if (rc == 0) {
        json_object_object_add(resp, "status", json_object_new_string("ok"));
        json_object_object_add(resp, "node_id", json_object_new_string(node.id));
        send_json(req, 200, resp);
        redis_sync_node_registered(ctx, &node);
    } else {
        json_object_object_add(resp, "status", json_object_new_string("error"));
        json_object_object_add(resp, "error", json_object_new_string("node table full"));
        send_json(req, 500, resp);
    }
    json_object_put(body);
    json_object_put(resp);
}

/* POST /v1/node/unregister
 * Body: { "id": "turbo-1" }
 */
static void api_handle_node_unregister(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;
    struct json_object *body = parse_request_body(req);
    if (!body) { send_json(req, 400, NULL); return; }

    struct json_object *jid = json_object_object_get(body, "id");
    if (!jid) { json_object_put(body); send_json(req, 400, NULL); return; }

    int rc = room_mgr_unregister_node(&ctx->mgr, json_object_get_string(jid));

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", rc == 0 ? json_object_new_string("ok")
                                                    : json_object_new_string("not_found"));
    send_json(req, rc == 0 ? 200 : 404, resp);
    json_object_put(body);
    json_object_put(resp);
}

/* POST /v1/node/heartbeat
 * Body: { "id": "turbo-1", "load": 50 }
 */
static void api_handle_node_heartbeat(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;
    struct json_object *body = parse_request_body(req);
    if (!body) { send_json(req, 400, NULL); return; }

    struct json_object *jid = json_object_object_get(body, "id");
    struct json_object *jload = json_object_object_get(body, "load");
    if (!jid) { json_object_put(body); send_json(req, 400, NULL); return; }

    uint32_t load = jload ? (uint32_t)json_object_get_int(jload) : 0;
    int rc = room_mgr_node_heartbeat(&ctx->mgr, json_object_get_string(jid), load);

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", rc == 0 ? json_object_new_string("ok")
                                                    : json_object_new_string("not_found"));
    send_json(req, rc == 0 ? 200 : 404, resp);
    json_object_put(body);
    json_object_put(resp);
}

/* GET /v1/node/list
 */
static void api_handle_node_list(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", json_object_new_string("ok"));

    struct json_object *nodes = json_object_new_array();
    for (uint32_t i = 0; i < ctx->mgr.node_count; i++) {
        conductor_node_t *n = &ctx->mgr.nodes[i];
        struct json_object *jn = json_object_new_object();
        json_object_object_add(jn, "id", json_object_new_string(n->id));
        json_object_object_add(jn, "ip", json_object_new_string(n->ip));
        json_object_object_add(jn, "port", json_object_new_int(n->port));
        json_object_object_add(jn, "admin_port", json_object_new_int(n->admin_port));
        json_object_object_add(jn, "alive", json_object_new_int(n->alive));
        json_object_object_add(jn, "load", json_object_new_int(n->load));
        json_object_object_add(jn, "capacity", json_object_new_int(n->capacity));
        json_object_array_add(nodes, jn);
    }
    json_object_object_add(resp, "nodes", nodes);
    json_object_object_add(resp, "total", json_object_new_int(ctx->mgr.node_count));
    send_json(req, 200, resp);
    json_object_put(resp);
}

/* ── Room handlers ─────────────────────────────────────────── */

/* POST /v1/room/create
 * Body: { "room_id": 1001 }  (optional, server assigns if omitted)
 */
static void api_handle_room_create(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;
    struct json_object *body = parse_request_body(req);
    if (!body) { send_json(req, 400, NULL); return; }

    struct json_object *jid = json_object_object_get(body, "room_id");
    uint32_t room_id = jid ? (uint32_t)json_object_get_int(jid) : 0;

    /* Auto-assign room_id if not provided */
    if (room_id == 0) {
        room_id = (uint32_t)(ctx->mgr.room_count + 1);
    }

    int rc = room_mgr_create_room(&ctx->mgr, room_id);

    struct json_object *resp = json_object_new_object();
    if (rc == 0) {
        conductor_room_t *room = room_mgr_get_room(&ctx->mgr, room_id);
        json_object_object_add(resp, "status", json_object_new_string("ok"));
        json_object_object_add(resp, "room_id", json_object_new_int(room_id));
        if (room) {
            json_object_object_add(resp, "node_id", json_object_new_string(room->node_id));
            json_object_object_add(resp, "node_ip", json_object_new_string(room->node_ip));
            json_object_object_add(resp, "node_port", json_object_new_int(room->node_port));
            redis_sync_room_created(ctx, room_id, room->node_id);
        }
        send_json(req, 200, resp);
    } else if (rc == -2) {
        conductor_room_t *room = room_mgr_get_room(&ctx->mgr, room_id);
        json_object_object_add(resp, "status", json_object_new_string("exists"));
        json_object_object_add(resp, "room_id", json_object_new_int(room_id));
        if (room) {
            json_object_object_add(resp, "node_id", json_object_new_string(room->node_id));
            json_object_object_add(resp, "node_ip", json_object_new_string(room->node_ip));
            json_object_object_add(resp, "node_port", json_object_new_int(room->node_port));
        }
        send_json(req, 200, resp);
    } else if (rc == -4) {
        json_object_object_add(resp, "status", json_object_new_string("error"));
        json_object_object_add(resp, "error", json_object_new_string("no available nodes"));
        send_json(req, 503, resp);
    } else {
        json_object_object_add(resp, "status", json_object_new_string("error"));
        json_object_object_add(resp, "error", json_object_new_string("failed to create room"));
        send_json(req, 500, resp);
    }

    json_object_put(body);
    json_object_put(resp);
}

/* DELETE /v1/room/destroy?room_id=1001
 */
static void api_handle_room_destroy(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_DELETE &&
        evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;

    const char *rid_str = get_query_param(req, "room_id");
    if (!rid_str) {
        struct json_object *body = parse_request_body(req);
        if (body) {
            struct json_object *jid = json_object_object_get(body, "room_id");
            if (jid) rid_str = json_object_get_string(jid);
        }
        if (body) json_object_put(body);
    }
    if (!rid_str) { send_json(req, 400, NULL); return; }

    uint32_t room_id = (uint32_t)atoi(rid_str);
    int rc = room_mgr_destroy_room(&ctx->mgr, room_id);

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", rc == 0 ? json_object_new_string("ok")
                                                    : json_object_new_string("not_found"));
    send_json(req, rc == 0 ? 200 : 404, resp);
    json_object_put(resp);

    if (rc == 0) {
        redis_sync_room_destroyed(ctx, room_id);
    }
}

/* GET /v1/room/info?room_id=1001
 */
static void api_handle_room_info(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;

    const char *rid_str = get_query_param(req, "room_id");
    if (!rid_str) { send_json(req, 400, NULL); return; }

    uint32_t room_id = (uint32_t)atoi(rid_str);
    conductor_room_t *room = room_mgr_get_room(&ctx->mgr, room_id);

    struct json_object *resp = json_object_new_object();
    if (room) {
        json_object_object_add(resp, "status", json_object_new_string("ok"));
        json_object_object_add(resp, "room_id", json_object_new_int(room->room_id));
        json_object_object_add(resp, "node_id", json_object_new_string(room->node_id));
        json_object_object_add(resp, "node_ip", json_object_new_string(room->node_ip));
        json_object_object_add(resp, "node_port", json_object_new_int(room->node_port));
        json_object_object_add(resp, "member_count", json_object_new_int(room->member_count));
        send_json(req, 200, resp);
    } else {
        json_object_object_add(resp, "status", json_object_new_string("not_found"));
        send_json(req, 404, resp);
    }
    json_object_put(resp);
}

/* GET /v1/room/list
 */
static void api_handle_room_list(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;

    conductor_room_t *list = NULL;
    uint32_t count = 0;
    room_mgr_get_room_list(&ctx->mgr, &list, &count);

    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", json_object_new_string("ok"));
    struct json_object *rooms = json_object_new_array();
    for (uint32_t i = 0; i < count; i++) {
        struct json_object *r = json_object_new_object();
        json_object_object_add(r, "room_id", json_object_new_int(list[i].room_id));
        json_object_object_add(r, "node_id", json_object_new_string(list[i].node_id));
        json_object_object_add(r, "node_ip", json_object_new_string(list[i].node_ip));
        json_object_object_add(r, "node_port", json_object_new_int(list[i].node_port));
        json_object_object_add(r, "member_count", json_object_new_int(list[i].member_count));
        json_object_array_add(rooms, r);
    }
    json_object_object_add(resp, "rooms", rooms);
    json_object_object_add(resp, "total", json_object_new_int(count));
    send_json(req, 200, resp);
    json_object_put(resp);
}

/* GET /v1/room/assign?room_id=1001
 * Returns the TurboNode that should handle this room
 */
static void api_handle_room_assign(struct evhttp_request *req, void *arg) {
    if (evhttp_request_get_command(req) != EVHTTP_REQ_GET &&
        evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
        send_json(req, 405, NULL);
        return;
    }
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;

    const char *rid_str = get_query_param(req, "room_id");
    if (!rid_str) { send_json(req, 400, NULL); return; }

    uint32_t room_id = (uint32_t)atoi(rid_str);
    conductor_node_t *node = NULL;
    int rc = room_mgr_assign_node(&ctx->mgr, room_id, &node);

    struct json_object *resp = json_object_new_object();
    if (rc == 0 && node) {
        json_object_object_add(resp, "status", json_object_new_string("ok"));
        json_object_object_add(resp, "node_id", json_object_new_string(node->id));
        json_object_object_add(resp, "node_ip", json_object_new_string(node->ip));
        json_object_object_add(resp, "node_port", json_object_new_int(node->port));
        json_object_object_add(resp, "node_admin_port", json_object_new_int(node->admin_port));
        send_json(req, 200, resp);
    } else {
        json_object_object_add(resp, "status", json_object_new_string("error"));
        json_object_object_add(resp, "error", json_object_new_string("no available nodes"));
        send_json(req, 503, resp);
    }
    json_object_put(resp);
}

/* GET /health
 */
static void api_handle_health(struct evhttp_request *req, void *arg) {
    (void)arg;
    struct json_object *resp = json_object_new_object();
    json_object_object_add(resp, "status", json_object_new_string("ok"));
    json_object_object_add(resp, "uptime", json_object_new_string("running"));
    send_json(req, 200, resp);
    json_object_put(resp);
}

/* ── Server start / stop ───────────────────────────────────── */

int api_server_start(conductor_ctx_t *ctx) {
    if (!ctx || !ctx->evbase) return -1;

    ctx->http_server = evhttp_new(ctx->evbase);
    if (!ctx->http_server) return -1;

    /* Register routes */
    evhttp_set_cb(ctx->http_server, "/health", api_handle_health, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/node/register", api_handle_node_register, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/node/unregister", api_handle_node_unregister, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/node/heartbeat", api_handle_node_heartbeat, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/node/list", api_handle_node_list, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/room/create", api_handle_room_create, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/room/destroy", api_handle_room_destroy, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/room/info", api_handle_room_info, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/room/list", api_handle_room_list, ctx);
    evhttp_set_cb(ctx->http_server, "/v1/room/assign", api_handle_room_assign, ctx);

    if (evhttp_bind_socket(ctx->http_server, "0.0.0.0", ctx->http_port) != 0) {
        evhttp_free(ctx->http_server);
        ctx->http_server = NULL;
        return -1;
    }

    return 0;
}

void api_server_stop(conductor_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->http_server) {
        evhttp_free(ctx->http_server);
        ctx->http_server = NULL;
    }
}
