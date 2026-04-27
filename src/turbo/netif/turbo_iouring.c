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
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <unistd.h>

#define IOURING_SQ_DEPTH  256
#define IOURING_CQ_DEPTH  512
#define BATCH_MAX          64
#define PKT_BUF_SIZE     2048

/* Per-packet recvmsg ancillary data (src_addr stored in msg_name) */
struct iouring_recv_ctx {
    struct rtp_packet   *pkt;
    struct iovec         iov;
    struct msghdr        msg;
    struct sockaddr_in6  src;
};

struct iouring_priv {
    struct io_uring ring;
    int             sock_fd;
    int             suspended;
    int             inflight_rx;   /* SQE recv entries submitted but no CQE yet */
};

/* Each queued recv carries a pointer to an rtp_packet as user_data */

static struct rtp_packet *iouring_alloc_pkt(struct turbo_netif *tif) {
    (void)tif;
    /* Extra space for iouring_recv_ctx appended after the data buffer */
    struct rtp_packet *p = calloc(1, sizeof(*p) + PKT_BUF_SIZE + sizeof(struct iouring_recv_ctx));
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

/* Submit one RECVMSG SQE to capture src_addr alongside packet data */
static void submit_recv_sqe(struct iouring_priv *priv, struct rtp_packet *pkt) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&priv->ring);
    if (!sqe) return;

    /* Embed recv context immediately after packet payload buffer */
    struct iouring_recv_ctx *ctx = (struct iouring_recv_ctx *)(pkt->data + PKT_BUF_SIZE);
    ctx->pkt            = pkt;
    ctx->iov.iov_base   = pkt->data;
    ctx->iov.iov_len    = PKT_BUF_SIZE;
    memset(&ctx->msg, 0, sizeof(ctx->msg));
    ctx->msg.msg_name    = &ctx->src;
    ctx->msg.msg_namelen = sizeof(ctx->src);
    ctx->msg.msg_iov     = &ctx->iov;
    ctx->msg.msg_iovlen  = 1;

    io_uring_prep_recvmsg(sqe, priv->sock_fd, &ctx->msg, 0);
    /* Use ctx as user_data so we can recover both pkt and src_addr */
    io_uring_sqe_set_data(sqe, ctx);
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

    /* Try to peek for existing CQEs first (no crash) */
    struct io_uring_cqe *cqe_check;
    int ret = io_uring_peek_cqe(&priv->ring, &cqe_check);
    if (ret < 0) {
        /* No CQE ready — submit and wait via ring fd */
        io_uring_submit(&priv->ring);
        int rfd = priv->ring.ring_fd;
        if (rfd >= 0) {
            struct pollfd pfd = { .fd = rfd, .events = POLLIN };
            poll(&pfd, 1, 5);  /* 5ms timeout */
        }
        ret = io_uring_peek_cqe(&priv->ring, &cqe_check);
        if (ret < 0) return 0;
    }

    struct io_uring_cqe *cqe;
    unsigned head;
    int n = 0;
    int total_consumed = 0;  /* ALL CQEs iterated — advance ring by this */
    int recv_consumed  = 0;  /* recv CQEs only — controls SQE resubmit count */

    io_uring_for_each_cqe(&priv->ring, head, cqe) {
        if (n >= max) break;
        total_consumed++;

        struct iouring_recv_ctx *ctx = io_uring_cqe_get_data(cqe);
        /* Distinguish send vs recv context via LSB tag:
         *   LSB=0 → recv (iouring_recv_ctx *)
         *   LSB=1 → send (iouring_send_ctx *), pointer tagged at SQE submission
         *   NULL  → legacy fire-and-forget (unused now but kept for safety) */
        uintptr_t raw = (uintptr_t)ctx;
        if (raw == 0) {
            continue;
        }
        if (raw & 1UL) {
            /* Send completion: free the send context */
            struct iouring_send_ctx *sctx = (struct iouring_send_ctx *)(raw & ~1UL);
            free(sctx);
            continue;
        }

        /* Recv completion: count it and update inflight_rx either way */
        recv_consumed++;
        priv->inflight_rx--;
        struct rtp_packet *p = ctx->pkt;

        if (cqe->res > 0) {
            p->len = (uint16_t)cqe->res;
            /* Copy captured source address into packet */
            if (ctx->msg.msg_namelen >= sizeof(struct sockaddr_in6))
                p->src_addr = ctx->src;
            else if (ctx->msg.msg_namelen >= sizeof(struct sockaddr_in)) {
                /* IPv4: convert to IPv4-mapped IPv6 */
                const struct sockaddr_in *s4 = (const struct sockaddr_in *)&ctx->src;
                memset(&p->src_addr, 0, sizeof(p->src_addr));
                p->src_addr.sin6_family = AF_INET6;
                p->src_addr.sin6_port   = s4->sin_port;
                p->src_addr.sin6_addr.s6_addr[10] = 0xff;
                p->src_addr.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&p->src_addr.sin6_addr.s6_addr[12], &s4->sin_addr, 4);
            }
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

/* Per-send msghdr — embedded in a small heap alloc for fire-and-forget lifetime */
struct iouring_send_ctx {
    struct msghdr  msg;
    struct iovec   iov;
    uint8_t        data[2048];  /* copy of packet payload */
};

static int iouring_send_burst(struct turbo_netif *tif,
                               struct rtp_packet **pkts, uint16_t count) {
    struct iouring_priv *priv = tif->priv;
    int submitted = 0;

    for (int i = 0; i < count; i++) {
        if (pkts[i]->len > 2048) continue;
        struct iouring_send_ctx *sctx = malloc(sizeof(*sctx));
        if (!sctx) break;

        memcpy(sctx->data, pkts[i]->data, pkts[i]->len);
        sctx->iov.iov_base = sctx->data;
        sctx->iov.iov_len  = pkts[i]->len;
        memset(&sctx->msg, 0, sizeof(sctx->msg));
        sctx->msg.msg_name    = &pkts[i]->dst_addr;
        sctx->msg.msg_namelen = sizeof(struct sockaddr_in6);
        sctx->msg.msg_iov     = &sctx->iov;
        sctx->msg.msg_iovlen  = 1;

        struct io_uring_sqe *sqe = io_uring_get_sqe(&priv->ring);
        if (!sqe) { free(sctx); break; }
        io_uring_prep_sendmsg(sqe, priv->sock_fd, &sctx->msg, 0);
        /* Tag LSB=1 to distinguish from recv ctx in CQE handler */
        io_uring_sqe_set_data(sqe, (void *)((uintptr_t)sctx | 1UL));
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
