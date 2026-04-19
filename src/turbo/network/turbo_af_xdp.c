#include "turbo_netif.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_xdp.h>
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

/* AF_XDP private data structure */
struct turbo_afxdp_priv {
    struct xsk_socket *xsk;
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    void *umem_area;
    struct xsk_umem *umem;
    uint32_t frame_size;
    uint32_t frame_headroom;
    uint32_t frame_tailroom;
    uint32_t num_frames;
    uint32_t frame_size_total;
};

/* AF_XDP frame structure */
struct turbo_afxdp_frame {
    uint64_t addr;
    uint32_t len;
    uint32_t options;
};

/* Convert turbo_packet to AF_XDP frame */
static inline struct turbo_afxdp_frame* pkt_to_frame(struct turbo_packet *pkt) {
    return (struct turbo_afxdp_frame*)pkt->priv;
}

/* Convert AF_XDP frame to turbo_packet */
static inline struct turbo_packet* frame_to_pkt(struct turbo_afxdp_priv *priv, uint64_t addr, uint32_t len) {
    struct turbo_packet *pkt = malloc(sizeof(struct turbo_packet));
    if (!pkt) return NULL;

    pkt->data = xsk_umem__get_data(priv->umem_area, addr);
    pkt->len = len;
    pkt->buf_len = priv->frame_size;
    pkt->priv = (void*)addr;  /* Store address as priv */

    return pkt;
}

/* AF_XDP backend operations */
static int turbo_afxdp_init(struct turbo_netif *netif, const char *ifname, uint16_t port) {
    struct turbo_afxdp_priv *priv;
    struct xsk_umem_config umem_cfg = {0};
    struct xsk_socket_config xsk_cfg = {0};
    int ret, ifindex;
    uint32_t frame_size_total;

    /* Allocate private data */
    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        return -ENOMEM;
    }

    /* Get interface index */
    ifindex = if_nametoindex(ifname);
    if (!ifindex) {
        free(priv);
        return -ENODEV;
    }

    /* Configure UMEM */
    priv->frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;
    priv->frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
    priv->frame_tailroom = XSK_UMEM__DEFAULT_FRAME_TAILROOM;
    priv->num_frames = XSK_RING_PROD__DEFAULT_NUM_DESCS * 2;
    priv->frame_size_total = priv->frame_size + priv->frame_headroom + priv->frame_tailroom;

    umem_cfg.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    umem_cfg.frame_size = priv->frame_size_total;
    umem_cfg.frame_headroom = priv->frame_headroom;

    /* Allocate UMEM area */
    priv->umem_area = aligned_alloc(getpagesize(),
                                   priv->num_frames * priv->frame_size_total);
    if (!priv->umem_area) {
        free(priv);
        return -ENOMEM;
    }

    /* Create UMEM */
    ret = xsk_umem__create(&priv->umem, priv->umem_area,
                          priv->num_frames * priv->frame_size_total,
                          &priv->fq, &priv->cq, &umem_cfg);
    if (ret) {
        free(priv->umem_area);
        free(priv);
        return -EIO;
    }

    /* Configure socket */
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.libbpf_flags = 0;
    xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;  /* Use SKB mode for compatibility */
    xsk_cfg.bind_flags = XDP_USE_NEED_WAKEUP;

    /* Create AF_XDP socket */
    ret = xsk_socket__create(&priv->xsk, ifname, 0, priv->umem,
                            &priv->rx, &priv->tx, &xsk_cfg);
    if (ret) {
        xsk_umem__delete(priv->umem);
        free(priv->umem_area);
        free(priv);
        return -EIO;
    }

    netif->priv = priv;
    netif->port = port;
    strncpy(netif->ifname, ifname, sizeof(netif->ifname) - 1);

    return 0;
}

static void turbo_afxdp_cleanup(struct turbo_netif *netif) {
    struct turbo_afxdp_priv *priv = netif->priv;

    if (priv) {
        netif->priv = NULL;  /* Prevent double-cleanup */
        if (priv->xsk) {
            xsk_socket__delete(priv->xsk);
            priv->xsk = NULL;
        }
        if (priv->umem) {
            xsk_umem__delete(priv->umem);
            priv->umem = NULL;
        }
        if (priv->umem_area) {
            free(priv->umem_area);
            priv->umem_area = NULL;
        }
        free(priv);
    }
}

static uint16_t turbo_afxdp_rx_burst(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts) {
    struct turbo_afxdp_priv *priv = netif->priv;
    uint32_t idx_rx = 0, idx_fq = 0;
    uint16_t nb_rx = 0;

    /* Receive packets */
    nb_rx = xsk_ring_cons__peek(&priv->rx, nb_pkts, &idx_rx);
    if (!nb_rx) {
        return 0;
    }

    /* Process received packets */
    for (uint16_t i = 0; i < nb_rx; i++) {
        const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&priv->rx, idx_rx + i);
        pkts[i] = frame_to_pkt(priv, desc->addr, desc->len);
        if (!pkts[i]) {
            /* Release remaining descriptors on allocation failure */
            xsk_ring_cons__release(&priv->rx, i);
            return i;
        }
    }

    /* Release descriptors */
    xsk_ring_cons__release(&priv->rx, nb_rx);

    /* Add frames back to fill queue */
    uint32_t fq_avail = xsk_prod_nb_free(&priv->fq, priv->num_frames);
    if (fq_avail >= nb_rx) {
        xsk_ring_prod__reserve(&priv->fq, nb_rx, &idx_fq);
        for (uint16_t i = 0; i < nb_rx; i++) {
            const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&priv->rx, idx_rx + i);
            *xsk_ring_prod__fill_addr(&priv->fq, idx_fq + i) = desc->addr;
        }
        xsk_ring_prod__submit(&priv->fq, nb_rx);
    }

    /* Wake up socket if needed */
    xsk_socket__fd(priv->xsk);
    // sendto() or poll() would be called here in a real implementation

    return nb_rx;
}

static uint16_t turbo_afxdp_tx_burst(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts) {
    struct turbo_afxdp_priv *priv = netif->priv;
    uint32_t idx_tx = 0;
    uint16_t nb_tx;

    /* Reserve TX descriptors */
    nb_tx = xsk_ring_prod__reserve(&priv->tx, nb_pkts, &idx_tx);
    if (!nb_tx) {
        return 0;
    }

    /* Fill TX descriptors */
    for (uint16_t i = 0; i < nb_tx; i++) {
        struct xdp_desc *desc = xsk_ring_prod__tx_desc(&priv->tx, idx_tx + i);
        desc->addr = (uint64_t)pkt_to_frame(pkts[i]);
        desc->len = pkts[i]->len;
        desc->options = 0;
    }

    /* Submit TX descriptors */
    xsk_ring_prod__submit(&priv->tx, nb_tx);

    /* Wake up socket if needed */
    xsk_socket__fd(priv->xsk);
    // sendto() or poll() would be called here in a real implementation

    return nb_tx;
}

static struct turbo_packet* turbo_afxdp_alloc_pkt(struct turbo_netif *netif, size_t size) {
    struct turbo_afxdp_priv *priv = netif->priv;
    uint64_t addr;
    uint32_t idx;

    /* Reserve from fill queue */
    if (xsk_ring_prod__reserve(&priv->fq, 1, &idx) != 1) {
        return NULL;
    }

    addr = *xsk_ring_prod__fill_addr(&priv->fq, idx);
    xsk_ring_prod__submit(&priv->fq, 1);

    return frame_to_pkt(priv, addr, 0);
}

static void turbo_afxdp_free_pkt(struct turbo_netif *netif, struct turbo_packet *pkt) {
    /* For zero-copy cloned packets, we only free the packet structure */
    /* The underlying UMEM frame is managed separately */
    free(pkt);
}

static struct turbo_packet* turbo_afxdp_clone_pkt(struct turbo_netif *netif, struct turbo_packet *pkt) {
    struct turbo_afxdp_priv *priv = netif->priv;
    uint64_t orig_addr = (uint64_t)pkt->priv;

    /* For AF_XDP, true zero-copy cloning means sharing the same UMEM frame */
    /* We create a new packet structure that references the same data */
    /* Note: In a real implementation, this would use reference counting */
    struct turbo_packet *cloned = malloc(sizeof(struct turbo_packet));
    if (!cloned) {
        return NULL;
    }

    /* Share the same data buffer (zero-copy) */
    cloned->data = pkt->data;
    cloned->len = pkt->len;
    cloned->buf_len = pkt->buf_len;
    cloned->priv = pkt->priv;  /* Share the same address */

    return cloned;
}

/* AF_XDP backend operations table */
const struct turbo_netif_ops turbo_afxdp_ops = {
    .init = turbo_afxdp_init,
    .cleanup = turbo_afxdp_cleanup,
    .rx_burst = turbo_afxdp_rx_burst,
    .tx_burst = turbo_afxdp_tx_burst,
    .alloc_pkt = turbo_afxdp_alloc_pkt,
    .free_pkt = turbo_afxdp_free_pkt,
    .clone_pkt = turbo_afxdp_clone_pkt,
};