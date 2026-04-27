/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Turbo network backend — selection, runtime degrade, worker loop.
 * Design doc §5.2, §5.3.2, §T2.5.
 */

#ifdef TURBO_FEATURES

#include "turbo_netif.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>

/* Defined in turbo.c; set by SIGUSR1 handler */
extern _Atomic int turbo_degrade_requested;

#define TURBO_RX_BURST 64

/* ------------------------------------------------------------------ */
/* Backend selection                                                    */
/* ------------------------------------------------------------------ */

static struct turbo_netif_ops *select_ops(int backend_type) {
    switch (backend_type) {
#ifdef TURBO_AFXDP
    case TURBO_BACKEND_AF_XDP:   return &turbo_afxdp_ops;
#endif
#ifdef TURBO_IOURING
    case TURBO_BACKEND_IO_URING: return &turbo_iouring_ops;
#endif
    case TURBO_BACKEND_EPOLL:
    default:                     return &turbo_epoll_ops;
    }
}

static const char *backend_name(int t) {
    switch (t) {
    case TURBO_BACKEND_AF_XDP:   return "AF_XDP";
    case TURBO_BACKEND_IO_URING: return "io_uring";
    case TURBO_BACKEND_EPOLL:    return "epoll";
    default:                     return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Init / close                                                         */
/* ------------------------------------------------------------------ */

int turbo_netif_init(struct turbo_netif *tif, int backend_type, int sock_fd) {
    memset(tif, 0, sizeof(*tif));
    tif->sock_fd      = sock_fd;
    tif->backend_type = backend_type;
    tif->health_status = TURBO_HEALTH_OK;
    atomic_store(&tif->running, 1);

    /* Try the requested backend, automatically fall back toward epoll */
    while (1) {
        tif->ops = select_ops(tif->backend_type);
        if (tif->ops->init(tif, NULL) == 0) {
            fprintf(stderr, "turbo: using %s backend\n",
                    backend_name(tif->backend_type));
            return 0;
        }
        fprintf(stderr, "turbo: %s init failed, trying fallback\n",
                backend_name(tif->backend_type));
        if (tif->backend_type >= TURBO_BACKEND_EPOLL)
            return -1;   /* epoll failed — unrecoverable */
        tif->backend_type++;
        /* After any non-epoll failure, skip io_uring and go straight to epoll.
         * io_uring crashes in some container environments despite passing
         * init validation — skip it to avoid segfaults in the worker. */
        if (tif->backend_type == TURBO_BACKEND_IO_URING)
            tif->backend_type = TURBO_BACKEND_EPOLL;
    }
}

void turbo_netif_close(struct turbo_netif *tif) {
    if (!tif || !tif->ops) return;
    atomic_store(&tif->running, 0);
    tif->ops->close(tif);
    tif->ops = NULL;
}

/* ------------------------------------------------------------------ */
/* Runtime degrade — one-way chain: AF_XDP → io_uring → epoll         */
/* ------------------------------------------------------------------ */

int turbo_netif_degrade(struct turbo_netif *tif,
                        enum turbo_degrade_trigger trigger) {
    int next = tif->backend_type + 1;
    if (next > TURBO_BACKEND_EPOLL) {
        /* Already at terminus; nothing to do */
        return -1;
    }

    fprintf(stderr, "turbo: degrading %s → %s (trigger=%d)\n",
            backend_name(tif->backend_type), backend_name(next), (int)trigger);

    /* Close current backend (releases priv and all kernel resources) */
    tif->ops->close(tif);

    tif->backend_type  = next;
    tif->ops           = select_ops(next);
    tif->health_status = TURBO_HEALTH_DEGRADED;
    tif->degraded_count++;

    if (tif->ops->init(tif, NULL) != 0) {
        fprintf(stderr, "turbo: %s init also failed, cascading\n",
                backend_name(next));
        /* Cascade to the next level */
        return turbo_netif_degrade(tif, trigger);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SIGUSR1 / admin-triggered degrade check                             */
/* ------------------------------------------------------------------ */

void turbo_netif_check_degrade_request(struct turbo_netif *tif) {
    if (atomic_exchange(&turbo_degrade_requested, 0))
        turbo_netif_degrade(tif, DEGRADE_MANUAL);
}

/* ------------------------------------------------------------------ */
/* Worker loop (runs in its own pthread)                               */
/* ------------------------------------------------------------------ */

/*
 * Weak hook: override in turbo.c once the fastpath/room layers are wired.
 * Default: drop the packet and return it to the backend pool.
 */
__attribute__((weak))
void turbo_process_packet(struct turbo_netif *tif, struct rtp_packet *pkt) {
    tif->dropped_packets++;
    tif->ops->free_pkt(tif, pkt);
}

void *turbo_worker_loop(void *arg) {
    struct turbo_netif *tif = arg;
    struct rtp_packet  *pkts[TURBO_RX_BURST];

    /* Block all signals in the worker thread; let the main thread handle them */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    while (atomic_load(&tif->running)) {
        turbo_netif_check_degrade_request(tif);

#ifdef TURBO_AFXDP
        if (tif->backend_type == TURBO_BACKEND_AF_XDP)
            turbo_afxdp_health_check(tif);
#endif

        int n = tif->ops->recv_pkts(tif, pkts, TURBO_RX_BURST);
        for (int i = 0; i < n; i++)
            turbo_process_packet(tif, pkts[i]);
    }

    return NULL;
}

#endif /* TURBO_FEATURES */
