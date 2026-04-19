#ifndef TURBO_AF_XDP_H
#define TURBO_AF_XDP_H

#include "turbo_netif.h"
#include <xsk/xsk.h>

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

/* AF_XDP backend operations table */
extern const struct turbo_netif_ops turbo_afxdp_ops;

/**
 * Load XDP program on interface
 * @param ifname Interface name
 * @param xdp_prog_path Path to compiled XDP program (.o)
 * @return 0 on success, negative error code on failure
 */
int turbo_afxdp_load_xdp_program(const char *ifname, const char *xdp_prog_path);

/**
 * Remove XDP program from interface
 * @param ifname Interface name
 * @return 0 on success, negative error code on failure
 */
int turbo_afxdp_remove_xdp_program(const char *ifname);

#endif /* TURBO_AF_XDP_H */
