/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Turbo network backend abstraction (design doc §5.1).
 * Supports epoll (fallback), io_uring (default), AF_XDP (optional).
 */

#ifndef TURBO_NETIF_H
#define TURBO_NETIF_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <netinet/in.h>

/* Backend type identifiers (matches Prometheus turbo_backend metric) */
#define TURBO_BACKEND_AF_XDP   1
#define TURBO_BACKEND_IO_URING 2
#define TURBO_BACKEND_EPOLL    3

/* Health status values */
#define TURBO_HEALTH_OK       0
#define TURBO_HEALTH_DEGRADED 1
#define TURBO_HEALTH_FAILED   2

/* Degrade trigger sources */
enum turbo_degrade_trigger {
    DEGRADE_MANUAL = 0,  /* SIGUSR1 / Admin API */
    DEGRADE_AUTO   = 1,  /* Fuse: RX ring saturation */
    DEGRADE_DRAIN  = 2,  /* Drain mode */
};

/* Packet descriptor — backend-agnostic */
struct rtp_packet {
    void            *data;          /* Payload start */
    uint16_t         len;           /* Payload length */
    uint16_t         headroom;      /* Bytes before data (for rewrite) */
    _Atomic uint32_t refcount;      /* Zero-copy clone ref count */
    uint64_t         timestamp;     /* Receive timestamp (ns) */
    uint32_t         alloc_id;      /* Associated allocation id */
    struct sockaddr_in6 src_addr;   /* Sender address */
    struct sockaddr_in6 dst_addr;   /* Destination (set before send) */
    void            *backend_priv;  /* Backend-specific (e.g., UMEM frame addr) */
};

struct turbo_netif;

/* Operations vtable — every backend implements all entries */
struct turbo_netif_ops {
    int  (*init)      (struct turbo_netif *tif, void *cfg);
    void (*close)     (struct turbo_netif *tif);
    void (*suspend)   (struct turbo_netif *tif);  /* Stop RX, keep resources */
    void (*resume)    (struct turbo_netif *tif);  /* Restart RX */

    /* Batch receive: returns ≤ max packets into pkts[], 0 on timeout/empty */
    int  (*recv_pkts) (struct turbo_netif *tif,
                       struct rtp_packet **pkts, uint16_t max);

    /* Batch send: returns count actually enqueued/sent */
    int  (*send_burst)(struct turbo_netif *tif,
                       struct rtp_packet **pkts, uint16_t count);

    struct rtp_packet *(*alloc_pkt)(struct turbo_netif *tif);
    void               (*free_pkt) (struct turbo_netif *tif,
                                    struct rtp_packet *pkt);
    /* Zero-copy if backend supports it; else memcpy + refcount++ */
    struct rtp_packet *(*clone_pkt)(struct turbo_netif *tif,
                                    struct rtp_packet *pkt);
};

/* Main per-netif state */
struct turbo_netif {
    struct turbo_netif_ops *ops;
    int      backend_type;      /* TURBO_BACKEND_* */
    int      health_status;     /* TURBO_HEALTH_* */
    int      sock_fd;           /* Shared UDP socket fd */
    _Atomic int running;        /* Worker loop flag */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t dropped_packets;
    uint64_t degraded_count;
    void    *priv;              /* Backend-specific private data */
};

/* Initialise netif with the requested backend type.
 * sock_fd must be a bound UDP socket (SO_REUSEPORT ok).
 * Returns 0 on success, -1 on failure (with automatic fallback toward epoll). */
int  turbo_netif_init(struct turbo_netif *tif, int backend_type, int sock_fd);

/* Close and release all backend resources; sets running=0 first. */
void turbo_netif_close(struct turbo_netif *tif);

/* Runtime downgrade: AF_XDP → io_uring → epoll (one-way per §5.3.2) */
int  turbo_netif_degrade(struct turbo_netif *tif,
                         enum turbo_degrade_trigger trigger);

/* Check atomic degrade request flag set by SIGUSR1 handler */
void turbo_netif_check_degrade_request(struct turbo_netif *tif);

/* Worker loop — runs in its own pthread; exits when running becomes 0 */
void *turbo_worker_loop(void *arg);

/* Packet dispatch hook; default implementation drops + frees the packet.
 * Override in turbo.c to wire the fastpath. */
void turbo_process_packet(struct turbo_netif *tif, struct rtp_packet *pkt);

/* AF_XDP periodic health check — integrated into worker loop */
#ifdef TURBO_AFXDP
void turbo_afxdp_health_check(struct turbo_netif *tif);
#endif

/* Per-backend ops registry */
extern struct turbo_netif_ops turbo_epoll_ops;
#ifdef TURBO_IOURING
extern struct turbo_netif_ops turbo_iouring_ops;
#endif
#ifdef TURBO_AFXDP
extern struct turbo_netif_ops turbo_afxdp_ops;
#endif

#endif /* TURBO_NETIF_H */
