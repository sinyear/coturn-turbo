#ifndef TURBO_DPDK_H
#define TURBO_DPDK_H

#include "turbo_netif.h"
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>

/* DPDK private data structure */
struct turbo_dpdk_priv {
    uint16_t port_id;
    struct rte_mempool *mbuf_pool;
    uint16_t nb_rx_queues;
    uint16_t nb_tx_queues;
};

/* DPDK backend operations table */
extern const struct turbo_netif_ops turbo_dpdk_ops;

/* Convert turbo_packet to rte_mbuf */
static inline struct rte_mbuf* pkt_to_mbuf(struct turbo_packet *pkt) {
    return (struct rte_mbuf *)pkt->priv;
}

/* Convert rte_mbuf to turbo_packet */
struct turbo_packet* mbuf_to_pkt(struct rte_mbuf *mbuf);

/**
 * Create DPDK mbuf pool
 * @param name Pool name
 * @param nb_mbufs Number of mbufs
 * @param socket_id NUMA socket ID
 * @return Mempool pointer or NULL
 */
struct rte_mempool* turbo_dpdk_create_mbuf_pool(const char *name,
                                                 uint32_t nb_mbufs,
                                                 unsigned int socket_id);

#endif /* TURBO_DPDK_H */
