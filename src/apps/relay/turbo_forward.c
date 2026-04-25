#include "turbo_forward.h"
#include "mainrelay.h"
#include "ns_turn_utils.h"
#include <stdlib.h>
#include <string.h>
#include "turbo_switch.h"
#include <netinet/ip.h>
#include <netinet/udp.h>

/* Initialize forwarding context */
void turbo_forward_init(struct turbo_forward_ctx *ctx,
                        struct turbo_room_mgr *room_mgr,
                        struct turbo_port_map *port_map,
                        struct turbo_netif *netif) {
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->room_mgr = room_mgr;
    ctx->port_map = port_map;
    ctx->netif = netif;
}

/* Process packet through forwarding pipeline */
int turbo_forward_process_packet(struct turbo_forward_ctx *ctx,
                                 struct turbo_packet *pkt) {
    if (!ctx || !pkt || !pkt->data) {
        return -1;
    }

    /* Extract five-tuple */
    struct turbo_five_tuple tuple = {0};
    if (turbo_extract_five_tuple(pkt->data, pkt->len, &tuple) != 0) {
        return -1;
    }

    /* Look up session in port map */
    uint32_t room_id = 0, member_id = 0;
    if (turbo_port_map_lookup(ctx->port_map, &tuple, &room_id, &member_id) != 0) {
        /* Fast-path miss */
        return -1;
    }

    /* Fast-path hit: broadcast to room members */
    int sent = turbo_room_broadcast(ctx->room_mgr, room_id, member_id, pkt);
    (void)sent; /* Could update stats here */

    return 0;
}

/* Fast-path hook for DTLS/UDP receive callbacks */
int turbo_forward_fast_path(struct turbo_forward_ctx *ctx,
                            const uint8_t *data, size_t len,
                            const struct sockaddr_in *src_addr) {
    (void)src_addr;
    if (!ctx || !data || len < 28) {
        return -1;
    }

    /* Extract five-tuple from raw IP packet */
    struct turbo_five_tuple tuple = {0};
    if (turbo_extract_five_tuple(data, len, &tuple) != 0) {
        return -1;
    }

    /* Look up session mapping */
    uint32_t room_id = 0, member_id = 0;
    if (turbo_port_map_lookup(ctx->port_map, &tuple, &room_id, &member_id) != 0) {
        return -1;
    }

    /* Create temporary packet for broadcast */
    struct turbo_packet pkt = {0};
    pkt.data = (void *)(uintptr_t)data;
    pkt.len = len;
    pkt.buf_len = len;

    /* Broadcast to room members */
    int sent = turbo_room_broadcast(ctx->room_mgr, room_id, member_id, &pkt);
    return (sent > 0) ? 0 : -1;
}

