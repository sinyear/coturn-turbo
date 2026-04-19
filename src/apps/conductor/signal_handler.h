#ifndef SIGNAL_HANDLER_H
#define SIGNAL_HANDLER_H

#include <stdint.h>
#include <stddef.h>
#include <event2/event.h>
#include "api_server.h"

/* WebSocket frame opcodes */
#define WS_OP_TEXT   0x1
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/* WebSocket client state */
#define WS_STATE_HANDSHAKE 0
#define WS_STATE_OPEN      1
#define WS_STATE_CLOSING   2

/**
 * WebSocket client connection
 */
typedef struct ws_client {
    evutil_socket_t fd;
    int ws_state;           /* 0=handshake, 1=open, 2=closing */
    int frame_state;
    uint8_t *payload_buf;
    char session_id[64];
    struct event *read_event;
    struct event *ping_event;
    struct ws_client *next;
} ws_client_t;

/**
 * Initialize global WebSocket context
 * @param ctx Conductor context (must have evbase and ws_port set)
 * @return 0 on success, -1 on failure
 */
int ws_server_start(conductor_ctx_t *ctx);

/**
 * Stop WebSocket server
 * @param ctx Conductor context
 */
void ws_server_stop(conductor_ctx_t *ctx);

/**
 * Dispatch a parsed WebSocket message
 * @param ctx Conductor context
 * @param text Message text
 * @param len Message length
 * @param client Client that sent the message
 */
void ws_dispatch_message(conductor_ctx_t *ctx, const char *text, size_t len,
                         ws_client_t *client);

/**
 * Get global WebSocket context (for use in callbacks)
 * @return Global context pointer
 */
conductor_ctx_t* ws_get_global_ctx(void);

#endif /* SIGNAL_HANDLER_H */
