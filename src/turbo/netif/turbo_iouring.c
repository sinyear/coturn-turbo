/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Turbo io_uring backend — default high-performance path (design doc §5.2).
 * Requires liburing ≥ 2.0 and kernel ≥ 5.6.
 */

#ifdef TURBO_IOURING

#include "turbo_netif.h"
#include <liburing.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define IOURING_SQ_DEPTH  256
#define IOURING_CQ_DEPTH  512
#define BATCH_MAX          64
#define PKT_BUF_SIZE     2048

struct iouring_priv {
    struct io_uring ring;
    int             sock_fd;
    int             suspended;
    int             inflight_rx;   /* SQE recv entries submitted but no CQE yet */
};

/* Each queued recv carries a pointer to an rtp_packet as user_data */

static struct rtp_packet *iouring_alloc_pkt(struct turbo_netif *tif) {
    (void)tif;
    struct rtp_packet *p = calloc(1, sizeof(*p) + PKT_BUF_SIZE);
    if (!p) return NULL;
    p->data = (uint8_t *)p + sizeof(*p);
    atomic_store(&p->refcount, 1);
    return p;
}

static void iouring_free_pkt(struct turbo_netif *tif, struct rtp_packet *pkt) {
    (void)tif;
    if (!pkt) return;
    if (atomic_fetch_sub(&pkt->refcount, 1) == 1)
        free(pkt);
}

static struct rtp_packet *iouring_clone_pkt(struct turbo_netif *tif,
                                             struct rtp_packet *src) {
    struct rtp_packet *dst = iouring_alloc_pkt(tif);
    if (!dst) return NULL;
    memcpy(dst->data, src->data, src->len);
    dst->len      = src->len;
    dst->src_addr = src->src_addr;
    dst->dst_addr = src->dst_addr;
    dst->alloc_id = src->alloc_id;
    return dst;
}

/* Submit one RECV SQE for a pre-allocated packet */
static void submit_recv_sqe(struct iouring_priv *priv, struct rtp_packet *pkt) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&priv->ring);
    if (!sqe) return;

    /* We use recvmsg-style via io_uring_prep_recv */
    io_uring_prep_recv(sqe, priv->sock_fd, pkt->data, PKT_BUF_SIZE, 0);
    /* store packet pointer so we can recover it from the CQE */
    io_uring_sqe_set_data(sqe, pkt);
    priv->inflight_rx++;
}

static int iouring_init(struct turbo_netif *tif, void *cfg) {
    (void)cfg;
    struct iouring_priv *priv = calloc(1, sizeof(*priv));
    if (!priv) return -1;

    priv->sock_fd = tif->sock_fd;

    struct io_uring_params params = {0};
    params.cq_entries = IOURING_CQ_DEPTH;
    if (io_uring_queue_init_params(IOURING_SQ_DEPTH, &priv->ring, &params) < 0) {
        free(priv); return -1;
    }

    tif->priv = priv;

    /* Pre-fill SQ with RECV SQEs */
    int pre = IOURING_SQ_DEPTH / 2;
    for (int i = 0; i < pre; i++) {
        struct rtp_packet *p = iouring_alloc_pkt(tif);
        if (!p) break;
        submit_recv_sqe(priv, p);
    }
    io_uring_submit(&priv->ring);
    return 0;
}

static void iouring_close(struct turbo_netif *tif) {
    struct iouring_priv *priv = tif->priv;
    if (!priv) return;

    /* Drain all remaining CQEs to avoid leaking packet allocations.
     * Count entries so we advance the ring by the actual number consumed,
     * not by inflight_rx (which can be negative due to send CQEs). */
    struct io_uring_cqe *cqe;
    unsigned head;
    int drained = 0;
    io_uring_for_each_cqe(&priv->ring, head, cqe) {
        struct rtp_packet *p = io_uring_cqe_get_data(cqe);
        if (p) iouring_free_pkt(tif, p);
        drained++;
    }
    io_uring_cq_advance(&priv->ring, drained);

    io_uring_queue_exit(&priv->ring);
    free(priv);
    tif->priv = NULL;
}

static void iouring_suspend(struct turbo_netif *tif) {
    struct iouring_priv *priv = tif->priv;
    if (priv) priv->suspended = 1;
    /* In-flight SQEs will complete; we just stop resubmitting */
}

static void iouring_resume(struct turbo_netif *tif) {
    struct iouring_priv *priv = tif->priv;
    if (!priv || !priv->suspended) return;
    priv->suspended = 0;

    /* Resubmit recv SQEs */
    int pre = IOURING_SQ_DEPTH / 2;
    for (int i = 0; i < pre; i++) {
        struct rtp_packet *p = iouring_alloc_pkt(tif);
        if (!p) break;
        submit_recv_sqe(priv, p);
    }
    io_uring_submit(&priv->ring);
}

static int iouring_recv_pkts(struct turbo_netif *tif,
                              struct rtp_packet **pkts, uint16_t max) {
    struct iouring_priv *priv = tif->priv;
    if (priv->suspended || max == 0) return 0;

    /* Submit and wait for at least 1 CQE (5ms timeout) */
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 5000000 };
    io_uring_submit_and_wait_timeout(&priv->ring, NULL, 1, &ts, NULL);

    struct io_uring_cqe *cqe;
    unsigned head;
    int n = 0;
    int total_consumed = 0;  /* ALL CQEs iterated — advance ring by this */
    int recv_consumed  = 0;  /* recv CQEs only — controls SQE resubmit count */

    io_uring_for_each_cqe(&priv->ring, head, cqe) {
        if (n >= max) break;
        total_consumed++;

        struct rtp_packet *p = io_uring_cqe_get_data(cqe);
        if (p == NULL) {
            /* Send completion (user_data=NULL, fire-and-forget).
             * io_uring still generates a CQE; consume it silently.
             * Do NOT decrement inflight_rx — sends were never counted there. */
            continue;
        }

        /* Recv completion: count it and update inflight_rx either way */
        recv_consumed++;
        priv->inflight_rx--;

        if (cqe->res > 0) {
            p->len = (uint16_t)cqe->res;
            pkts[n++] = p;
        } else {
            /* Error or zero-length: free packet but still advance the CQE.
             * NOT advancing was the original bug that caused double-free. */
            iouring_free_pkt(tif, p);
        }
    }
    /* Advance by ALL iterated entries (recv success + recv error + send).
     * Advancing only by n (previous code) left error/send CQEs stranded,
     * causing double-free when they were re-seen on the next call. */
    io_uring_cq_advance(&priv->ring, total_consumed);

    if (n > 0) tif->rx_packets += n;

    /* Resubmit recv SQEs for every consumed recv slot (success + error)
     * so that inflight_rx and the SQ depth stay stable. */
    if (!priv->suspended) {
        for (int i = 0; i < recv_consumed; i++) {
            struct rtp_packet *p = iouring_alloc_pkt(tif);
            if (!p) break;
            submit_recv_sqe(priv, p);
        }
        if (recv_consumed > 0) io_uring_submit(&priv->ring);
    }

    return n;
}

static int iouring_send_burst(struct turbo_netif *tif,
                               struct rtp_packet **pkts, uint16_t count) {
    struct iouring_priv *priv = tif->priv;
    int submitted = 0;

    for (int i = 0; i < count; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&priv->ring);
        if (!sqe) break;
        io_uring_prep_send(sqe, priv->sock_fd, pkts[i]->data, pkts[i]->len, 0);
        io_uring_sqe_set_data(sqe, NULL);  /* fire-and-forget */
        submitted++;
    }

    if (submitted > 0) {
        io_uring_submit(&priv->ring);
        tif->tx_packets += submitted;
    }
    return submitted;
}

struct turbo_netif_ops turbo_iouring_ops = {
    .init       = iouring_init,
    .close      = iouring_close,
    .suspend    = iouring_suspend,
    .resume     = iouring_resume,
    .recv_pkts  = iouring_recv_pkts,
    .send_burst = iouring_send_burst,
    .alloc_pkt  = iouring_alloc_pkt,
    .free_pkt   = iouring_free_pkt,
    .clone_pkt  = iouring_clone_pkt,
};

#endif /* TURBO_IOURING */
