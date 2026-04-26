/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unicast forward and broadcast clone implementation.
 * Design doc §T3.3 — adapted to rtp_packet interface.
 */

#include "turbo_switch.h"
#include <string.h>

int turbo_switch_forward(struct turbo_netif *netif,
                          struct rtp_packet *pkt,
                          const struct sockaddr_in6 *dst_addr) {
    struct rtp_packet *clone = netif->ops->clone_pkt(netif, pkt);
    if (!clone) return -1;

    clone->dst_addr = *dst_addr;
    netif->ops->send_burst(netif, &clone, 1);
    netif->ops->free_pkt(netif, clone);
    return 0;
}

int turbo_switch_broadcast(struct turbo_netif *netif,
                            struct rtp_packet *pkt,
                            const struct sockaddr_in6 *dst_addrs,
                            int count,
                            struct rtp_packet **tx_array,
                            int tx_array_size) {
    int n = 0;
    for (int i = 0; i < count && n < tx_array_size; i++) {
        struct rtp_packet *clone = netif->ops->clone_pkt(netif, pkt);
        if (!clone) continue;
        clone->dst_addr = dst_addrs[i];
        tx_array[n++] = clone;
    }
    return n;
}
