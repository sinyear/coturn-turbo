#ifndef TURBO_CORE_H
#define TURBO_CORE_H

#include "turbo_netif.h"
#include "turbo_port.h"
#include "turbo_room.h"
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

/* Maximum burst size for RX/TX */
#define TURBO_RX_BURST 64

/* Poll loop interval in microseconds (for non-DPDK busy-wait) */
#define TURBO_POLL_INTERVAL_US 10

/**
 * Turbo engine state
 */
struct turbo_core {
    struct turbo_netif *netif;       /* Network backend */
    struct turbo_room_mgr *room_mgr; /* Room manager */
    struct turbo_port_map port_map;  /* Single-port multiplexing */

    pthread_t poll_thread;           /* Poll thread */
    atomic_int running;              /* Engine running flag */

    /* Stats */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t lookup_hits;
    uint64_t lookup_misses;
};

/**
 * Initialize turbo core engine
 * @param core Engine state (caller-allocated)
 * @param netif Network interface
 * @param room_mgr Room manager
 * @return 0 on success, negative error code on failure
 */
int turbo_core_init(struct turbo_core *core,
                    struct turbo_netif *netif,
                    struct turbo_room_mgr *room_mgr);

/**
 * Start turbo core poll loop (blocking in detached thread)
 * @param core Engine state
 * @return 0 on success, negative error code on failure
 */
int turbo_core_start(struct turbo_core *core);

/**
 * Stop turbo core poll loop
 * @param core Engine state
 */
void turbo_core_stop(struct turbo_core *core);

/**
 * Cleanup turbo core resources
 * @param core Engine state
 */
void turbo_core_cleanup(struct turbo_core *core);

/**
 * Process a single received packet through the turbo fast path.
 * This extracts the five-tuple, looks up the room/member mapping,
 * and broadcasts to room members if found.
 *
 * Called by the poll loop or by the DTLS fast-path hook.
 *
 * @param core Engine state
 * @param pkt Received packet (data starts at IP header)
 * @return 0 on success, -1 on miss/drop
 */
int turbo_core_process_packet(struct turbo_core *core, struct turbo_packet *pkt);

/**
 * Register a session mapping (called when a client joins a room)
 * @param core Engine state
 * @param room_id Room ID
 * @param member_id Member ID
 * @param src_addr Source IP (network byte order)
 * @param src_port Source port (network byte order)
 * @return 0 on success, negative error code on failure
 */
int turbo_core_register_session(struct turbo_core *core,
                                uint32_t room_id, uint32_t member_id,
                                uint32_t src_addr, uint16_t src_port);

/**
 * Register a session mapping (IPv6-capable variant)
 * @param core Engine state
 * @param room_id Room ID
 * @param member_id Member ID
 * @param src_addr Source IP address bytes (4 for IPv4, 16 for IPv6)
 * @param addr_len Length of address (4 or 16)
 * @param src_port Source port (network byte order)
 * @param af Address family (TURBO_AF_INET or TURBO_AF_INET6)
 * @return 0 on success, negative error code on failure
 */
int turbo_core_register_session_af(struct turbo_core *core,
                                   uint32_t room_id, uint32_t member_id,
                                   const uint8_t *src_addr, size_t addr_len,
                                   uint16_t src_port, uint8_t af);

/**
 * Unregister a session mapping (called when a client leaves a room)
 * @param core Engine state
 * @param src_addr Source IP (network byte order)
 * @param src_port Source port (network byte order)
 */
void turbo_core_unregister_session(struct turbo_core *core,
                                   uint32_t src_addr, uint16_t src_port);

/**
 * Unregister a session mapping (IPv6-capable variant)
 * @param core Engine state
 * @param src_addr Source IP address bytes
 * @param addr_len Length of address (4 or 16)
 * @param src_port Source port (network byte order)
 * @param af Address family (TURBO_AF_INET or TURBO_AF_INET6)
 */
void turbo_core_unregister_session_af(struct turbo_core *core,
                                      const uint8_t *src_addr, size_t addr_len,
                                      uint16_t src_port, uint8_t af);

/**
 * Get engine stats
 */
typedef struct turbo_core_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t lookup_hits;
    uint64_t lookup_misses;
} turbo_core_stats_t;

void turbo_core_get_stats(struct turbo_core *core, turbo_core_stats_t *stats);
void turbo_core_reset_stats(struct turbo_core *core);

#endif /* TURBO_CORE_H */
