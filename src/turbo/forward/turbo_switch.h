/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Zero-copy unicast forward and broadcast clone helper.
 * Adapted to the rtp_packet interface (design doc §T3.3).
 */

#ifndef TURBO_SWITCH_H
#define TURBO_SWITCH_H

#include "../netif/turbo_netif.h"
#include <stdint.h>
#include <netinet/in.h>

/*
 * Forward a single packet to dst_addr:
 *   1. Clone the packet (zero-copy if backend supports it).
 *   2. Set clone->dst_addr.
 *   3. Call send_burst(netif, &clone, 1).
 *   4. Free clone.
 * The original pkt is NOT consumed; caller must free it.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
int turbo_switch_forward(struct turbo_netif *netif,
                          struct rtp_packet *pkt,
                          const struct sockaddr_in6 *dst_addr);

/*
 * Broadcast pkt to count destinations in dst_addrs[].
 * For each destination:
 *   1. Clone pkt (zero-copy).
 *   2. Set dst_addr.
 *   3. Batch into tx_array (up to tx_array_size entries).
 * Caller submits the tx_array via send_burst and frees clones.
 *
 * Returns number of clones placed in tx_array.
 */
int turbo_switch_broadcast(struct turbo_netif *netif,
                            struct rtp_packet *pkt,
                            const struct sockaddr_in6 *dst_addrs,
                            int count,
                            struct rtp_packet **tx_array,
                            int tx_array_size);

#endif /* TURBO_SWITCH_H */
