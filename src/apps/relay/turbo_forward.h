#ifndef TURBO_FORWARD_APP_H
#define TURBO_FORWARD_APP_H

#include "turbo_netif.h"
#include "turbo_port.h"
#include "turbo_room.h"
#include "turbo_switch.h"
#include <stdint.h>

/**
 * Forwarding pipeline for turbo relay.
 *
 * This module connects the receive path (from DTLS/UDP listener)
 * to the SFU broadcast path through the turbo room manager.
 *
 * Usage:
 *   1. Initialize with turbo_forward_init()
 *   2. Call turbo_forward_process_packet() for each received packet
 *   3. On fast-path hit, packet is broadcast to room members
 *   4. On fast-path miss, fall back to native STUN/TURN processing
 */

/* Forwarding context */
struct turbo_forward_ctx {
    struct turbo_room_mgr *room_mgr;
    struct turbo_port_map *port_map;
    struct turbo_netif *netif;
};

/**
 * Initialize forwarding context
 * @param ctx Forwarding context
 * @param room_mgr Room manager
 * @param port_map Port map for session lookup
 * @param netif Network interface
 */
void turbo_forward_init(struct turbo_forward_ctx *ctx,
                        struct turbo_room_mgr *room_mgr,
                        struct turbo_port_map *port_map,
                        struct turbo_netif *netif);

/**
 * Process a received packet through the forwarding pipeline.
 * Extracts five-tuple, looks up session mapping, and broadcasts
 * if the session belongs to a room.
 *
 * @param ctx Forwarding context
 * @param pkt Received packet (data starts at IP header)
 * @return 0 if broadcast sent, -1 if fast-path miss (fall back to STUN/TURN)
 */
int turbo_forward_process_packet(struct turbo_forward_ctx *ctx,
                                 struct turbo_packet *pkt);

/**
 * Fast-path hook for DTLS/UDP receive callbacks.
 * This is the entry point from the native turnserver receive path.
 *
 * @param ctx Forwarding context
 * @param data Packet data (raw IP packet)
 * @param len Packet length
 * @param src_addr Source address
 * @return 0 if handled by turbo, -1 to fall back to native processing
 */
int turbo_forward_fast_path(struct turbo_forward_ctx *ctx,
                            const uint8_t *data, size_t len,
                            const struct sockaddr_in *src_addr);

#endif /* TURBO_FORWARD_APP_H */
