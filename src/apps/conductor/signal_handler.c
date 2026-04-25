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
#include <sys/types.h>
#include <sys/uio.h>
#include <json-c/json.h>
#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <openssl/sha.h>

/* ── Portability: htonll ──────────────────────────────────── */

#ifndef htonll
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define htonll(x) ((((uint64_t)htonl((x) & 0xFFFFFFFF)) << 32) | htonl((x) >> 32))
#elif defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN) && __BYTE_ORDER == __LITTLE_ENDIAN
#define htonll(x) ((((uint64_t)htonl((x) & 0xFFFFFFFF)) << 32) | htonl((x) >> 32))
#else
#define htonll(x) (x)
#endif
#endif

#ifndef ntohll
#define ntohll(x) htonll(x)
#endif

/* ── WebSocket frame opcodes (RFC 6455) ───────────────────── */

#define WS_OP_TEXT   0x1
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* ── Forward declarations ──────────────────────────────────── */

static void ws_accept_cb(evutil_socket_t fd, short what, void *arg);
static void ws_read_cb(evutil_socket_t fd, short what, void *arg);
static void ws_ping_cb(evutil_socket_t fd, short what, void *arg);
static int  ws_send_frame(ws_client_t *client, uint8_t opcode, const uint8_t *payload, size_t len);
static void ws_close_client(ws_client_t *client);

/* ── Accept new TCP connection ─────────────────────────────── */

static void ws_accept_cb(evutil_socket_t fd, short what, void *arg) {
    conductor_ctx_t *ctx = (conductor_ctx_t *)arg;
    (void)what;

    struct sockaddr_storage peer;
    socklen_t peer_len = sizeof(peer);
    evutil_socket_t cfd = accept(fd, (struct sockaddr *)&peer, &peer_len);
    if (cfd < 0) return;

    evutil_make_socket_nonblocking(cfd);

    ws_client_t *client = calloc(1, sizeof(ws_client_t));
    if (!client) {
        evutil_closesocket(cfd);
        return;
    }

    client->fd = cfd;
    client->ws_state = 0;  /* handshake */
    client->frame_state = 0;
    client->payload_buf = NULL;

    /* Generate session ID */
    snprintf(client->session_id, sizeof(client->session_id),
             "ws-%p", (void *)(uintptr_t)cfd);

    /* Create read event — ctx is the event_base, client is the callback arg.
       We store ctx in a thread-local / static. See below. */
    client->read_event = event_new(ctx->evbase, cfd, EV_READ | EV_PERSIST,
                                    ws_read_cb, client);
    if (!client->read_event) {
        free(client);
        evutil_closesocket(cfd);
        return;
    }

    /* Create ping event */
    struct timeval ping_tv = { CONDUCTOR_WS_PING_INTERVAL, 0 };
    client->ping_event = event_new(ctx->evbase, -1, EV_PERSIST,
                                    ws_ping_cb, client);
    if (client->ping_event) {
        event_add(client->ping_event, &ping_tv);
    }

    event_add(client->read_event, NULL);
}

/* ── Parse WebSocket handshake ─────────────────────────────── */

static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-5AB0AC8B4E7B";

static int ws_do_handshake(ws_client_t *client, const uint8_t *data, size_t len) {
    char buf[4096];
    size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, data, copy);
    buf[copy] = '\0';

    char *key_start = strstr(buf, "Sec-WebSocket-Key: ");
    if (!key_start) return -1;

    char *nl = strchr(key_start, '\r');
    if (!nl) nl = strchr(key_start, '\n');
    if (!nl) return -1;

    /* Extract the key value */
    char *val_start = strchr(key_start, ':');
    if (!val_start) return -1;
    val_start++;
    while (*val_start == ' ') val_start++;

    size_t key_len = (size_t)(nl - val_start);
    if (key_len == 0 || key_len > 128) return -1;

    /* Compute accept: base64(SHA1(key + GUID)) */
    char concat[256] = {0};
    memcpy(concat, val_start, key_len);
    strcpy(concat + key_len, WS_GUID);

    unsigned char sha1[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)concat, strlen(concat), sha1);

    /* Base64 encode */
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char encoded[32];
    int ei = 0;
    for (int i = 0; i < SHA_DIGEST_LENGTH; i += 3) {
        uint32_t n = (uint32_t)sha1[i] << 16;
        if (i + 1 < SHA_DIGEST_LENGTH) n |= (uint32_t)sha1[i + 1] << 8;
        if (i + 2 < SHA_DIGEST_LENGTH) n |= (uint32_t)sha1[i + 2];
        encoded[ei++] = b64[(n >> 18) & 0x3F];
        encoded[ei++] = b64[(n >> 12) & 0x3F];
        encoded[ei++] = (i + 1 < SHA_DIGEST_LENGTH) ? b64[(n >> 6) & 0x3F] : '=';
        encoded[ei++] = (i + 2 < SHA_DIGEST_LENGTH) ? b64[n & 0x3F] : '=';
    }
    encoded[ei] = '\0';

    /* Send HTTP 101 */
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", encoded);

    if (send(client->fd, resp, rlen, 0) < 0) return -1;

    client->ws_state = 1;  /* open */
    return 0;
}

/* ── Read from WebSocket ───────────────────────────────────── */

static void ws_read_cb(evutil_socket_t fd, short what, void *arg) {
    ws_client_t *client = (ws_client_t *)arg;
    if (!client) return;

    if (what & (EV_CLOSED | EV_EOF)) {
        ws_close_client(client);
        return;
    }

    uint8_t buf[8192];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        ws_close_client(client);
        return;
    }

    /* If still in handshake phase, parse HTTP upgrade request */
    if (client->ws_state == 0) {
        if (ws_do_handshake(client, buf, n) != 0) {
            ws_close_client(client);
        }
        return;
    }

    /* Parse WebSocket frame (RFC 6455 minimal parser) */
    uint8_t opcode;
    const uint8_t *payload;
    size_t payload_len;
    int free_payload = 0;

    if (n < 2) return;

    opcode = buf[0] & 0x0F;
    uint8_t mask_flag = (buf[1] >> 7) & 1;
    uint64_t plen = buf[1] & 0x7F;
    size_t offset = 2;

    if (plen == 126) {
        if (n < 4) return;
        plen = (uint16_t)buf[2] << 8 | buf[3];
        offset = 4;
    } else if (plen == 127) {
        if (n < 10) return;
        uint64_t tmp = 0;
        memcpy(&tmp, buf + 2, 8);
        plen = ntohll(tmp);
        offset = 10;
    }

    uint8_t mask[4] = {0};
    if (mask_flag) {
        if (n < (ssize_t)(offset + 4)) return;
        memcpy(mask, buf + offset, 4);
        offset += 4;
    }

    if (n < (ssize_t)(offset + plen)) return;

    payload = buf + offset;

    /* Unmask client-to-server frames */
    if (mask_flag && plen > 0) {
        uint8_t *um = malloc(plen);
        if (!um) return;
        for (uint64_t i = 0; i < plen; i++) {
            um[i] = payload[i] ^ mask[i % 4];
        }
        payload = um;
        free_payload = 1;
    }

    /* Dispatch by opcode */
    switch (opcode) {
    case WS_OP_TEXT: {
        /* Find conductor_ctx from the client's event base association.
           We use a static pointer set during ws_server_start. */
        conductor_ctx_t *ctx = ws_get_global_ctx();
        if (ctx && plen > 0) {
            ws_dispatch_message(ctx, (const char *)payload, plen, client);
        }
        break;
    }
    case WS_OP_PING:
        ws_send_frame(client, WS_OP_PONG, payload, plen);
        break;
    case WS_OP_CLOSE:
        ws_close_client(client);
        break;
    default:
        break;
    }

    if (free_payload) {
        free((void *)payload);
    }
}

/* ── Global ctx accessor for read callback ─────────────────── */

static conductor_ctx_t *g_ws_ctx = NULL;

conductor_ctx_t* ws_get_global_ctx(void) {
    return g_ws_ctx;
}

/* ── Dispatch WebSocket message ────────────────────────────── */

void ws_dispatch_message(conductor_ctx_t *ctx, const char *text, size_t len,
                         ws_client_t *client) {
    struct json_object *msg = json_tokener_parse_len(text, (int)len);
    if (!msg) return;

    struct json_object *jtype = json_object_object_get(msg, "type");
    const char *type = jtype ? json_object_get_string(jtype) : "";

    struct json_object *resp = json_object_new_object();

    if (strcmp(type, "create_room") == 0) {
        struct json_object *jid = json_object_object_get(msg, "room_id");
        uint32_t room_id = jid ? (uint32_t)json_object_get_int(jid) : 0;
        if (room_id == 0) {
            room_id = (uint32_t)(ctx->mgr.room_count + 1);
        }

        int rc = room_mgr_create_room(&ctx->mgr, room_id);
        json_object_object_add(resp, "type", json_object_new_string("room_created"));

        if (rc == 0 || rc == -2) {
            conductor_room_t *room = room_mgr_get_room(&ctx->mgr, room_id);
            json_object_object_add(resp, "status", json_object_new_string("ok"));
            json_object_object_add(resp, "room_id", json_object_new_int(room_id));
            if (room) {
                json_object_object_add(resp, "node_id", json_object_new_string(room->node_id));
                json_object_object_add(resp, "node_ip", json_object_new_string(room->node_ip));
                json_object_object_add(resp, "node_port", json_object_new_int(room->node_port));
            }
        } else {
            json_object_object_add(resp, "status", json_object_new_string("error"));
            json_object_object_add(resp, "message", json_object_new_string("no available nodes"));
        }

    } else if (strcmp(type, "join") == 0) {
        struct json_object *jid = json_object_object_get(msg, "room_id");
        uint32_t room_id = jid ? (uint32_t)json_object_get_int(jid) : 0;

        json_object_object_add(resp, "type", json_object_new_string("joined"));
        if (room_id > 0) {
            conductor_room_t *room = room_mgr_get_room(&ctx->mgr, room_id);
            if (room) {
                json_object_object_add(resp, "status", json_object_new_string("ok"));
                json_object_object_add(resp, "room_id", json_object_new_int(room_id));
                json_object_object_add(resp, "node_id", json_object_new_string(room->node_id));
                json_object_object_add(resp, "node_ip", json_object_new_string(room->node_ip));
                json_object_object_add(resp, "node_port", json_object_new_int(room->node_port));
            } else {
                json_object_object_add(resp, "status", json_object_new_string("not_found"));
            }
        } else {
            json_object_object_add(resp, "status", json_object_new_string("error"));
        }

    } else if (strcmp(type, "leave") == 0) {
        json_object_object_add(resp, "type", json_object_new_string("left"));
        json_object_object_add(resp, "status", json_object_new_string("ok"));

    } else if (strcmp(type, "ping") == 0) {
        json_object_object_add(resp, "type", json_object_new_string("pong"));

    } else {
        json_object_object_add(resp, "type", json_object_new_string("error"));
        json_object_object_add(resp, "message", json_object_new_string("unknown message type"));
    }

    const char *json_str = json_object_to_json_string(resp);
    ws_send_frame(client, WS_OP_TEXT, (const uint8_t *)json_str, strlen(json_str));

    json_object_put(msg);
    json_object_put(resp);
}

/* ── Send WebSocket frame ──────────────────────────────────── */

static int ws_send_frame(ws_client_t *client, uint8_t opcode, const uint8_t *payload, size_t len) {
    uint8_t header[14];
    size_t header_len = 2;

    header[0] = 0x80 | opcode;  /* FIN + opcode */

    if (len < 126) {
        header[1] = (uint8_t)len;
    } else if (len < 65536) {
        header[1] = 126;
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)len;
        header_len = 4;
    } else {
        header[1] = 127;
        uint64_t nlen = htonll(len);
        memcpy(header + 2, &nlen, 8);
        header_len = 10;
    }

    struct iovec iov[2];
    int niov = 1;
    iov[0].iov_base = header;
    iov[0].iov_len = header_len;
    if (payload && len > 0) {
        iov[1].iov_base = (void *)payload;
        iov[1].iov_len = len;
        niov = 2;
    }

    ssize_t sent = writev(client->fd, iov, niov);
    return sent < 0 ? -1 : 0;
}

/* ── Ping callback ─────────────────────────────────────────── */

static void ws_ping_cb(evutil_socket_t fd, short what, void *arg) {
    ws_client_t *client = (ws_client_t *)arg;
    (void)fd;
    (void)what;
    if (client && client->ws_state == 1) {
        ws_send_frame(client, WS_OP_PING, NULL, 0);
    }
}

/* ── Close client ──────────────────────────────────────────── */

static void ws_close_client(ws_client_t *client) {
    if (!client) return;

    if (client->ws_state == 1) {
        uint8_t close_frame[4] = {0};
        close_frame[0] = 0x80 | WS_OP_CLOSE;
        close_frame[1] = 2;
        close_frame[2] = 0x03;
        close_frame[3] = 0xE8;
        (void)send(client->fd, close_frame, 4, 0);
    }

    if (client->read_event) {
        event_del(client->read_event);
        event_free(client->read_event);
    }
    if (client->ping_event) {
        event_del(client->ping_event);
        event_free(client->ping_event);
    }

    evutil_closesocket(client->fd);
    client->ws_state = 2;
    if (client->payload_buf) free(client->payload_buf);
    free(client);
}

/* ── Server start / stop ───────────────────────────────────── */

int ws_server_start(conductor_ctx_t *ctx) {
    if (!ctx || !ctx->evbase) return -1;

    evutil_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    evutil_make_socket_nonblocking(fd);
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->ws_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        evutil_closesocket(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        evutil_closesocket(fd);
        return -1;
    }

    g_ws_ctx = ctx;

    ctx->ws_listen_fd = fd;
    ctx->ws_accept_event = event_new(ctx->evbase, fd, EV_READ | EV_PERSIST,
                                      ws_accept_cb, ctx);
    if (!ctx->ws_accept_event) {
        evutil_closesocket(fd);
        g_ws_ctx = NULL;
        return -1;
    }

    event_add(ctx->ws_accept_event, NULL);
    return 0;
}

void ws_server_stop(conductor_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->ws_accept_event) {
        event_del(ctx->ws_accept_event);
        event_free(ctx->ws_accept_event);
        ctx->ws_accept_event = NULL;
    }
    if (ctx->ws_listen_fd >= 0) {
        evutil_closesocket(ctx->ws_listen_fd);
        ctx->ws_listen_fd = -1;
    }
    g_ws_ctx = NULL;
}
