#ifndef TURBO_AF_XDP_H
#define TURBO_AF_XDP_H

#include "turbo_netif.h"
#include <xdp/xsk.h>

/* Compatibility: XDP flags may not be defined in older headers */
#ifndef XDP_FLAGS_SKB_MODE
#define XDP_FLAGS_SKB_MODE 0U
#endif
#ifndef XDP_FLAGS_DRV_MODE
#define XDP_FLAGS_DRV_MODE (1U << 1)
#endif

/**
 * AF_XDP operating modes
 */
enum afxdp_mode {
	AFXDP_MODE_AUTO = 0,  /* Try DRV, fallback to SKB */
	AFXDP_MODE_DRV,       /* Native XDP driver mode (zero-copy) */
	AFXDP_MODE_SKB,       /* SKB kernel fallback mode */
};

/**
 * Resolve xdp_flags for a given mode.
 * Returns XDP_FLAGS_DRV_MODE, XDP_FLAGS_SKB_MODE, or XDP_FLAGS_UPDATE_IF_NOEXIST for auto-detection.
 */
static inline uint32_t afxdp_mode_to_flags(enum afxdp_mode mode)
{
	switch (mode) {
	case AFXDP_MODE_DRV:  return XDP_FLAGS_DRV_MODE;
	case AFXDP_MODE_SKB:  return XDP_FLAGS_SKB_MODE;
	case AFXDP_MODE_AUTO:
	default:              return XDP_FLAGS_DRV_MODE; /* try DRV first */
	}
}

/**
 * Convert mode enum to human-readable string
 */
static inline const char* afxdp_mode_to_str(enum afxdp_mode mode)
{
	switch (mode) {
	case AFXDP_MODE_AUTO: return "auto";
	case AFXDP_MODE_DRV:  return "drv";
	case AFXDP_MODE_SKB:  return "skb";
	default:              return "unknown";
	}
}

/**
 * AF_XDP private data structure.
 * Only one definition — used by turbo_af_xdp.c internals.
 */
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

	/* Extended fields for optimized AF_XDP support */
	struct xdp_program *xdp_prog;       /* Loaded XDP program (libxdp) */
	uint32_t *frame_refcount;           /* Per-frame reference counts for safe clone */
	enum afxdp_mode mode;               /* Actual resolved mode (DRV or SKB) */
	uint16_t target_port;               /* Port for XDP filtering */
	int xdp_prog_fd;                    /* XDP program file descriptor */
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

/**
 * Parse afxdp mode string ("auto", "drv", "skb")
 * @param str Mode string
 * @return Corresponding enum value, AFXDP_MODE_AUTO on parse failure
 */
enum afxdp_mode afxdp_parse_mode(const char *str);

#endif /* TURBO_AF_XDP_H */
