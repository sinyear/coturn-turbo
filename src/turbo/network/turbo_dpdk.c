#include "turbo_netif.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_version.h>

/* DPDK private data structure */
struct turbo_dpdk_priv {
    uint16_t port_id;
    struct rte_mempool *mbuf_pool;
    uint16_t nb_rx_queues;
    uint16_t nb_tx_queues;
};

/* Convert turbo_packet to rte_mbuf */
static inline struct rte_mbuf* pkt_to_mbuf(struct turbo_packet *pkt) {
    return (struct rte_mbuf*)pkt->priv;
}

/* Convert rte_mbuf to turbo_packet */
struct turbo_packet* mbuf_to_pkt(struct rte_mbuf *mbuf) {
    struct turbo_packet *pkt = malloc(sizeof(struct turbo_packet));
    if (!pkt) return NULL;

    pkt->data = rte_pktmbuf_mtod(mbuf, void*);
    pkt->len = rte_pktmbuf_data_len(mbuf);
    pkt->buf_len = rte_pktmbuf_data_len(mbuf);
    pkt->priv = mbuf;

    return pkt;
}

/* DPDK backend operations */
static int turbo_dpdk_init(struct turbo_netif *netif, const char *ifname, uint16_t port) {
    struct turbo_dpdk_priv *priv;
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_dev_info dev_info;
    int ret;

    /* Allocate private data */
    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        return -ENOMEM;
    }

    /* Initialize DPDK EAL if not already done */
    if (!rte_eal_process_type()) {
        char *argv[] = {"turbo", "-l", "0-3", "--proc-type=primary"};
        int argc = sizeof(argv) / sizeof(argv[0]);
        ret = rte_eal_init(argc, argv);
        if (ret < 0) {
            free(priv);
            return -EIO;
        }
    }

    /* Find port by name */
    priv->port_id = RTE_MAX_ETHPORTS;
    for (uint16_t i = 0; i < RTE_MAX_ETHPORTS; i++) {
        if (rte_eth_dev_is_valid_port(i)) {
            struct rte_eth_dev_info dev_info;
            rte_eth_dev_info_get(i, &dev_info);
            if (strcmp(dev_info.device->name, ifname) == 0) {
                priv->port_id = i;
                break;
            }
        }
    }

    if (priv->port_id == RTE_MAX_ETHPORTS) {
        free(priv);
        return -ENODEV;
    }

    /* Create mbuf pool */
    priv->mbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", 8192, 256, 0,
                                             RTE_MBUF_DEFAULT_BUF_SIZE,
                                             rte_socket_id());
    if (!priv->mbuf_pool) {
        free(priv);
        return -ENOMEM;
    }

    /* Configure port with hardware checksum offload */
    rte_eth_dev_info_get(priv->port_id, &dev_info);
    priv->nb_rx_queues = 1;
    priv->nb_tx_queues = 1;

    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    /* Enable hardware checksum offload if supported */
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
    }
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
    }

    ret = rte_eth_dev_configure(priv->port_id, priv->nb_rx_queues,
                               priv->nb_tx_queues, &port_conf);
    if (ret < 0) {
        rte_mempool_free(priv->mbuf_pool);
        free(priv);
        return -EIO;
    }

    /* Setup RX queue */
    ret = rte_eth_rx_queue_setup(priv->port_id, 0, 128,
                                rte_eth_dev_socket_id(priv->port_id),
                                NULL, priv->mbuf_pool);
    if (ret < 0) {
        rte_eth_dev_stop(priv->port_id);
        rte_mempool_free(priv->mbuf_pool);
        free(priv);
        return -EIO;
    }

    /* Setup TX queue */
    ret = rte_eth_tx_queue_setup(priv->port_id, 0, 128,
                                rte_eth_dev_socket_id(priv->port_id), NULL);
    if (ret < 0) {
        rte_eth_dev_stop(priv->port_id);
        rte_mempool_free(priv->mbuf_pool);
        free(priv);
        return -EIO;
    }

    /* Start port */
    ret = rte_eth_dev_start(priv->port_id);
    if (ret < 0) {
        rte_eth_dev_stop(priv->port_id);
        rte_mempool_free(priv->mbuf_pool);
        free(priv);
        return -EIO;
    }

    /* Enable promiscuous mode */
    rte_eth_promiscuous_enable(priv->port_id);

    netif->priv = priv;
    netif->port = port;
    strncpy(netif->ifname, ifname, sizeof(netif->ifname) - 1);

    return 0;
}

static void turbo_dpdk_cleanup(struct turbo_netif *netif) {
    struct turbo_dpdk_priv *priv = netif->priv;

    if (priv) {
        netif->priv = NULL;  /* Prevent double-cleanup */
        if (priv->port_id < RTE_MAX_ETHPORTS) {
            rte_eth_dev_stop(priv->port_id);
            priv->port_id = RTE_MAX_ETHPORTS;
        }
        if (priv->mbuf_pool) {
            rte_mempool_free(priv->mbuf_pool);
            priv->mbuf_pool = NULL;
        }
        free(priv);
    }
}

static uint16_t turbo_dpdk_rx_burst(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts) {
    struct turbo_dpdk_priv *priv = netif->priv;
    struct rte_mbuf *mbufs[nb_pkts];
    uint16_t nb_rx;

    nb_rx = rte_eth_rx_burst(priv->port_id, 0, mbufs, nb_pkts);

    for (uint16_t i = 0; i < nb_rx; i++) {
        pkts[i] = mbuf_to_pkt(mbufs[i]);
        if (!pkts[i]) {
            /* Free remaining mbufs on allocation failure */
            for (uint16_t j = i; j < nb_rx; j++) {
                rte_pktmbuf_free(mbufs[j]);
            }
            return i;
        }
    }

    return nb_rx;
}

static uint16_t turbo_dpdk_tx_burst(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts) {
    struct turbo_dpdk_priv *priv = netif->priv;
    struct rte_mbuf *mbufs[nb_pkts];

    for (uint16_t i = 0; i < nb_pkts; i++) {
        mbufs[i] = pkt_to_mbuf(pkts[i]);
        mbufs[i]->data_len = (uint16_t)pkts[i]->len;
        mbufs[i]->pkt_len = (uint32_t)pkts[i]->len;
    }

    return rte_eth_tx_burst(priv->port_id, 0, mbufs, nb_pkts);
}

static struct turbo_packet* turbo_dpdk_alloc_pkt(struct turbo_netif *netif, size_t size) {
    struct turbo_dpdk_priv *priv = netif->priv;
    struct rte_mbuf *mbuf;

    mbuf = rte_pktmbuf_alloc(priv->mbuf_pool);
    if (!mbuf) {
        return NULL;
    }

    return mbuf_to_pkt(mbuf);
}

static void turbo_dpdk_free_pkt(struct turbo_netif *netif, struct turbo_packet *pkt) {
    struct rte_mbuf *mbuf = pkt_to_mbuf(pkt);
    rte_pktmbuf_free(mbuf);
    free(pkt);
}

static struct turbo_packet* turbo_dpdk_clone_pkt(struct turbo_netif *netif, struct turbo_packet *pkt) {
    struct rte_mbuf *orig_mbuf = pkt_to_mbuf(pkt);
    struct rte_mbuf *new_mbuf;

    new_mbuf = rte_pktmbuf_clone(orig_mbuf, netif->priv->mbuf_pool);
    if (!new_mbuf) {
        return NULL;
    }

    return mbuf_to_pkt(new_mbuf);
}

/* DPDK backend operations table */
const struct turbo_netif_ops turbo_dpdk_ops = {
    .init = turbo_dpdk_init,
    .cleanup = turbo_dpdk_cleanup,
    .rx_burst = turbo_dpdk_rx_burst,
    .tx_burst = turbo_dpdk_tx_burst,
    .alloc_pkt = turbo_dpdk_alloc_pkt,
    .free_pkt = turbo_dpdk_free_pkt,
    .clone_pkt = turbo_dpdk_clone_pkt,
};