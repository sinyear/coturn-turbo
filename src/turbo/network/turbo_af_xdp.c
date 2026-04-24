#include "turbo_af_xdp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <net/if.h>
#include <linux/if_xdp.h>
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

/* Compatibility: XSK_UMEM__DEFAULT_FRAME_TAILROOM was never a standard macro */
#ifndef XSK_UMEM__DEFAULT_FRAME_TAILROOM
#define XSK_UMEM__DEFAULT_FRAME_TAILROOM 0
#endif

/* Compatibility: XDP_FLAGS_SKB_MODE may not be defined in older libxdp */
#ifndef XDP_FLAGS_SKB_MODE
#define XDP_FLAGS_SKB_MODE 0U
#endif

/* Compatibility: XDP_FLAGS_DRV_MODE may not be defined in older libxdp */
#ifndef XDP_FLAGS_DRV_MODE
#define XDP_FLAGS_DRV_MODE (1 << 1)
#endif

/* Default XDP program search paths */
#define XDP_PROG_PATH_INSTALLED "/usr/share/turnserver/xdp_prog.o"
#define XDP_PROG_PATH_SOURCE    "src/turbo/network/xdp_prog.o"
#define XDP_PROG_PATH_LOCAL      "./xdp_prog.o"

/* Convert turbo_packet to AF_XDP frame */
static inline struct turbo_afxdp_frame* pkt_to_frame(struct turbo_packet *pkt)
{
	return (struct turbo_afxdp_frame*)pkt->priv;
}

/* Convert AF_XDP frame to turbo_packet */
static inline struct turbo_packet* frame_to_pkt(struct turbo_afxdp_priv *priv, uint64_t addr, uint32_t len)
{
	struct turbo_packet *pkt = malloc(sizeof(struct turbo_packet));
	if (!pkt) return NULL;

	pkt->data = xsk_umem__get_data(priv->umem_area, addr);
	pkt->len = len;
	pkt->buf_len = priv->frame_size;
	pkt->priv = (void*)(uintptr_t)addr;  /* Store address as priv */

	return pkt;
}

/* Parse afxdp mode string */
enum afxdp_mode afxdp_parse_mode(const char *str)
{
	if (!str) return AFXDP_MODE_AUTO;
	if (strcmp(str, "auto") == 0) return AFXDP_MODE_AUTO;
	if (strcmp(str, "drv") == 0) return AFXDP_MODE_DRV;
	if (strcmp(str, "skb") == 0) return AFXDP_MODE_SKB;
	fprintf(stderr, "af_xdp: unknown mode '%s', using auto\n", str);
	return AFXDP_MODE_AUTO;
}

/* Find the XDP program .o file */
static const char* find_xdp_prog(const char *hint_path)
{
	if (hint_path && hint_path[0] && access(hint_path, F_OK) == 0)
		return hint_path;
	if (access(XDP_PROG_PATH_INSTALLED, F_OK) == 0)
		return XDP_PROG_PATH_INSTALLED;
	if (access(XDP_PROG_PATH_SOURCE, F_OK) == 0)
		return XDP_PROG_PATH_SOURCE;
	if (access(XDP_PROG_PATH_LOCAL, F_OK) == 0)
		return XDP_PROG_PATH_LOCAL;
	return NULL;
}

/* Load and attach XDP program to the interface */
static int load_xdp_prog(struct turbo_afxdp_priv *priv, int ifindex, enum afxdp_mode mode)
{
	const char *xdp_path = find_xdp_prog(NULL);
	struct xdp_program *prog = NULL;
	int ret;

	if (!xdp_path) {
		fprintf(stderr, "af_xdp: xdp_prog.o not found in any search path\n");
		fprintf(stderr, "af_xdp: searched: %s, %s, %s\n",
			XDP_PROG_PATH_INSTALLED, XDP_PROG_PATH_SOURCE, XDP_PROG_PATH_LOCAL);
		return -ENOENT;
	}

	/* Open the XDP program */
	prog = xdp_program__open_file(xdp_path, "xdp", NULL);
	if (!prog || libxdp_get_error(prog)) {
		ret = prog ? libxdp_get_error(prog) : -EINVAL;
		fprintf(stderr, "af_xdp: failed to open XDP program '%s' (errno=%d: %s)\n",
			xdp_path, -ret, strerror(-ret));
		return ret;
	}

	/* Attach XDP program to the interface */
	ret = xdp_program__attach(prog, ifindex,
				  (mode == AFXDP_MODE_DRV) ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE,
				  0);
	if (ret) {
		fprintf(stderr, "af_xdp: failed to attach XDP program (errno=%d: %s)\n",
			ret, strerror(-ret));
		xdp_program__close(prog);
		return ret;
	}

	priv->xdp_prog = prog;
	priv->mode = mode;

	fprintf(stderr, "af_xdp: XDP program '%s' loaded in %s mode\n",
		xdp_path, afxdp_mode_to_str(mode));
	return 0;
}

/* Remove XDP program from interface */
static void remove_xdp_prog(struct turbo_afxdp_priv *priv)
{
	if (priv->xdp_prog) {
		xdp_program__close(priv->xdp_prog);
		priv->xdp_prog = NULL;
	}
}

/* Initialize frame reference counts */
static int init_frame_refcount(struct turbo_afxdp_priv *priv)
{
	priv->frame_refcount = calloc(priv->num_frames, sizeof(uint32_t));
	if (!priv->frame_refcount) {
		fprintf(stderr, "af_xdp: failed to allocate frame refcount array\n");
		return -ENOMEM;
	}
	return 0;
}

/* Atomic increment for frame refcount */
static inline void frame_ref_inc(struct turbo_afxdp_priv *priv, uint64_t addr)
{
	uint32_t idx = (uint32_t)(addr / priv->frame_size_total);
	if (idx < priv->num_frames)
		__sync_fetch_and_add(&priv->frame_refcount[idx], 1);
}

/* Atomic decrement for frame refcount; returns true if refcount reached 0 */
static inline bool frame_ref_dec(struct turbo_afxdp_priv *priv, uint64_t addr)
{
	uint32_t idx = (uint32_t)(addr / priv->frame_size_total);
	if (idx < priv->num_frames)
		return __sync_fetch_and_sub(&priv->frame_refcount[idx], 1) == 1;
	return true;
}

/* AF_XDP backend operations */
static int turbo_afxdp_init(struct turbo_netif *netif, const char *ifname, uint16_t port)
{
	struct turbo_afxdp_priv *priv;
	struct xsk_umem_config umem_cfg = {0};
	struct xsk_socket_config xsk_cfg = {0};
	int ret, ifindex;
	enum afxdp_mode requested_mode = AFXDP_MODE_AUTO;

	/* Check for mode override via environment (mainrelay.c sets this) */
	const char *mode_env = getenv("TURBO_AFXDP_MODE");
	if (mode_env)
		requested_mode = afxdp_parse_mode(mode_env);

	/* Allocate private data */
	priv = calloc(1, sizeof(*priv));
	if (!priv) {
		fprintf(stderr, "af_xdp: failed to allocate private data\n");
		return -ENOMEM;
	}

	priv->target_port = port;

	/* Get interface index */
	ifindex = if_nametoindex(ifname);
	if (!ifindex) {
		fprintf(stderr, "af_xdp: interface '%s' not found (errno=%d: %s)\n",
			ifname, errno, strerror(errno));
		fprintf(stderr, "af_xdp: verify the interface exists with 'ip link show %s'\n", ifname);
		free(priv);
		return -ENODEV;
	}
	fprintf(stderr, "af_xdp: interface '%s' -> ifindex=%d\n", ifname, ifindex);

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
	priv->umem_area = aligned_alloc(sysconf(_SC_PAGESIZE),
				       priv->num_frames * priv->frame_size_total);
	if (!priv->umem_area) {
		fprintf(stderr, "af_xdp: failed to allocate UMEM area (%lu bytes)\n",
			(unsigned long)(priv->num_frames * priv->frame_size_total));
		free(priv);
		return -ENOMEM;
	}

	/* Initialize frame reference counts */
	ret = init_frame_refcount(priv);
	if (ret) {
		free(priv->umem_area);
		free(priv);
		return ret;
	}

	/* Create UMEM */
	/* Increase RLIMIT_MEMLOCK to allow UMEM area to be locked into RAM.
	 * Required on kernels < 5.11 and still needed for AF_XDP on 5.15+. */
	{
		struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };
		if (setrlimit(RLIMIT_MEMLOCK, &rlim) != 0) {
			fprintf(stderr, "af_xdp: setrlimit(RLIMIT_MEMLOCK) failed (errno=%d: %s)\n",
				errno, strerror(errno));
			fprintf(stderr, "af_xdp: try running 'ulimit -l unlimited' as root before starting\n");
		}
	}
	ret = xsk_umem__create(&priv->umem, priv->umem_area,
			      priv->num_frames * priv->frame_size_total,
			      &priv->fq, &priv->cq, &umem_cfg);
	if (ret) {
		fprintf(stderr, "af_xdp: xsk_umem__create failed (errno=%d: %s)\n", ret, strerror(-ret));
		free(priv->frame_refcount);
		free(priv->umem_area);
		free(priv);
		return -EIO;
	}

	/*
	 * XDP Mode Selection:
	 *   AUTO: Try DRV first, fallback to SKB
	 *   DRV:  Native XDP driver mode (zero-copy, requires driver support)
	 *   SKB:  SKB kernel mode (compatible but may have higher latency)
	 *
	 * CRITICAL: An XDP program MUST be loaded to filter packets.
	 * Without it, the AF_XDP socket would capture ALL traffic on the
	 * bound queue, causing SSH and other services to become unreachable.
	 */
	enum afxdp_mode mode = requested_mode;

	if (mode == AFXDP_MODE_AUTO) {
		/* Try DRV mode first */
		ret = load_xdp_prog(priv, ifindex, AFXDP_MODE_DRV);
		if (ret == 0) {
			fprintf(stderr, "af_xdp: using DRV mode (native zero-copy)\n");
		} else {
			/* Fallback to SKB mode */
			fprintf(stderr, "af_xdp: DRV mode failed (errno=%d), falling back to SKB\n", -ret);
			ret = load_xdp_prog(priv, ifindex, AFXDP_MODE_SKB);
			if (ret == 0) {
				fprintf(stderr, "af_xdp: using SKB mode (kernel fallback)\n");
			} else {
				fprintf(stderr, "af_xdp: both DRV and SKB modes failed\n");
			}
		}
	} else {
		/* Explicit mode requested */
		ret = load_xdp_prog(priv, ifindex, mode);
		if (ret) {
			fprintf(stderr, "af_xdp: failed to load XDP program in %s mode\n",
				afxdp_mode_to_str(mode));
		}
	}

	if (ret != 0) {
		fprintf(stderr, "af_xdp: XDP program load failed — other services (SSH, etc.) may be affected\n");
		/* Continue anyway but warn — the socket creation below will still work
		 * but without XDP filtering, the AF_XDP socket may capture all traffic */
	}

	/* Configure socket */
	xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
	xsk_cfg.libbpf_flags = 0;
	xsk_cfg.xdp_flags = afxdp_mode_to_flags(priv->mode);
	xsk_cfg.bind_flags = XDP_USE_NEED_WAKEUP;

	/* Create AF_XDP socket using the modern shared API */
	ret = xsk_socket__create_shared(&priv->xsk, ifname, 0, priv->umem,
				       &priv->rx, &priv->tx, &priv->fq, &priv->cq,
				       &xsk_cfg);
	if (ret) {
		fprintf(stderr, "af_xdp: xsk_socket__create_shared failed (errno=%d: %s)\n",
			ret, strerror(-ret));
		fprintf(stderr, "af_xdp: this usually means XDP is not supported on '%s'\n", ifname);
		fprintf(stderr, "af_xdp: check driver XDP support: 'ethtool -k %s | grep xdp'\n", ifname);
		remove_xdp_prog(priv);
		xsk_umem__delete(priv->umem);
		free(priv->frame_refcount);
		free(priv->umem_area);
		free(priv);
		return -EIO;
	}

	fprintf(stderr, "af_xdp: successfully initialized interface '%s' (ifindex=%d, mode=%s)\n",
		ifname, ifindex, afxdp_mode_to_str(priv->mode));

	netif->priv = priv;
	netif->port = port;
	strncpy(netif->ifname, ifname, sizeof(netif->ifname) - 1);

	return 0;
}

static void turbo_afxdp_cleanup(struct turbo_netif *netif)
{
	struct turbo_afxdp_priv *priv = netif->priv;

	if (priv) {
		netif->priv = NULL;  /* Prevent double-cleanup */
		if (priv->xsk) {
			xsk_socket__delete(priv->xsk);
			priv->xsk = NULL;
		}
		remove_xdp_prog(priv);
		if (priv->umem) {
			xsk_umem__delete(priv->umem);
			priv->umem = NULL;
		}
		if (priv->frame_refcount) {
			free(priv->frame_refcount);
			priv->frame_refcount = NULL;
		}
		if (priv->umem_area) {
			free(priv->umem_area);
			priv->umem_area = NULL;
		}
		free(priv);
	}
}

static uint16_t turbo_afxdp_rx_burst(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts)
{
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
		/* Initialize refcount for received frame (ownership transferred to packet) */
		uint64_t addr = desc->addr;
		uint32_t frame_idx = (uint32_t)(addr / priv->frame_size_total);
		if (frame_idx < priv->num_frames)
			priv->frame_refcount[frame_idx] = 1;
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

	return nb_rx;
}

static uint16_t turbo_afxdp_tx_burst(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts)
{
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
		uint64_t addr = (uint64_t)(uintptr_t)pkt_to_frame(pkts[i])->addr;
		desc->addr = addr;
		desc->len = pkts[i]->len;
		desc->options = 0;
	}

	/* Submit TX descriptors */
	xsk_ring_prod__submit(&priv->tx, nb_tx);

	/* Wake up socket if needed */
	if (xsk_ring_prod__needs_wakeup(&priv->tx))
		sendto(xsk_socket__fd(priv->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

	return nb_tx;
}

static struct turbo_packet* turbo_afxdp_alloc_pkt(struct turbo_netif *netif, size_t size)
{
	struct turbo_afxdp_priv *priv = netif->priv;
	uint64_t addr;
	uint32_t idx;
	(void)size;

	/* Reserve from fill queue */
	if (xsk_ring_prod__reserve(&priv->fq, 1, &idx) != 1) {
		return NULL;
	}

	addr = *xsk_ring_prod__fill_addr(&priv->fq, idx);
	xsk_ring_prod__submit(&priv->fq, 1);

	return frame_to_pkt(priv, addr, 0);
}

static void turbo_afxdp_free_pkt(struct turbo_netif *netif, struct turbo_packet *pkt)
{
	if (!pkt) return;

	struct turbo_afxdp_priv *priv = netif->priv;
	uint64_t addr = (uint64_t)(uintptr_t)pkt->priv;

	/* Decrement reference count; only return frame to fill queue when count reaches 0 */
	if (priv && frame_ref_dec(priv, addr)) {
		/* Frame no longer referenced — return to fill queue */
		uint32_t idx;
		if (xsk_ring_prod__reserve(&priv->fq, 1, &idx) == 1) {
			*xsk_ring_prod__fill_addr(&priv->fq, idx) = addr;
			xsk_ring_prod__submit(&priv->fq, 1);
		}
	}

	free(pkt);
}

static struct turbo_packet* turbo_afxdp_clone_pkt(struct turbo_netif *netif, struct turbo_packet *pkt)
{
	(void)netif;

	/* Zero-copy clone: share the same UMEM frame with reference counting.
	 * The cloned packet references the same data buffer; the frame is only
	 * returned to the fill queue when the last reference is freed.
	 */
	struct turbo_packet *cloned = malloc(sizeof(struct turbo_packet));
	if (!cloned) {
		return NULL;
	}

	/* Share the same data buffer (zero-copy) */
	cloned->data = pkt->data;
	cloned->len = pkt->len;
	cloned->buf_len = pkt->buf_len;
	cloned->priv = pkt->priv;  /* Share the same address */

	/* Increment reference count for the shared frame */
	struct turbo_afxdp_priv *priv = netif->priv;
	if (priv)
		frame_ref_inc(priv, (uint64_t)(uintptr_t)pkt->priv);

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

/* Public implementations of the stubs declared in the header */

int turbo_afxdp_load_xdp_program(const char *ifname, const char *xdp_prog_path)
{
	if (!ifname) return -EINVAL;

	const char *path = find_xdp_prog(xdp_prog_path);
	if (!path) {
		fprintf(stderr, "af_xdp: xdp program not found\n");
		return -ENOENT;
	}

	int ifindex = if_nametoindex(ifname);
	if (!ifindex) {
		fprintf(stderr, "af_xdp: interface '%s' not found\n", ifname);
		return -ENODEV;
	}

	struct xdp_program *prog = xdp_program__open_file(path, "xdp", NULL);
	if (!prog || libxdp_get_error(prog)) {
		fprintf(stderr, "af_xdp: failed to open XDP program\n");
		return -EINVAL;
	}

	int ret = xdp_program__attach(prog, ifindex, XDP_FLAGS_DRV_MODE, 0);
	if (ret) {
		/* Try SKB mode as fallback */
		ret = xdp_program__attach(prog, ifindex, XDP_FLAGS_SKB_MODE, 0);
		if (ret) {
			xdp_program__close(prog);
			fprintf(stderr, "af_xdp: failed to attach XDP program\n");
			return ret;
		}
	}

	fprintf(stderr, "af_xdp: XDP program '%s' loaded on '%s'\n", path, ifname);
	xdp_program__close(prog);
	return 0;
}

int turbo_afxdp_remove_xdp_program(const char *ifname)
{
	if (!ifname) return -EINVAL;

	int ifindex = if_nametoindex(ifname);
	if (!ifindex) return -ENODEV;

	/* Use libxdp public API to detach XDP programs */
	struct xdp_multiprog *mp = xdp_multiprog__get_from_ifindex(ifindex);
	if (!mp || libxdp_get_error(mp)) {
		if (mp) xdp_multiprog__close(mp);
		return 0;  /* No program loaded */
	}

	enum xdp_attach_mode mode = xdp_multiprog__attach_mode(mp);

	/* Iterate through all XDP programs on the interface and detach them */
	struct xdp_program *prev = NULL;
	int ret = 0;
	struct xdp_program *prog;

	while ((prog = xdp_multiprog__next_prog(prev, mp)) != NULL) {
		__u32 prog_id = xdp_program__id(prog);
		int r = xdp_program__detach(prog, ifindex, mode, prog_id);
		if (r && r != -ESRCH)
			ret = r;
		prev = prog;
	}

	xdp_multiprog__close(mp);

	if (ret == 0)
		fprintf(stderr, "af_xdp: XDP program removed from '%s'\n", ifname);

	return ret;
}
