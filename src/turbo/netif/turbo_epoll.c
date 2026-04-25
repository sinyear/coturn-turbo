/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Turbo epoll backend — fallback / degrade terminus (design doc §5.2).
 * Uses recvmmsg/sendmmsg for batch I/O without extra kernel dependencies.
 */

#include "turbo_netif.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define EPOLL_MAX_EVENTS 1
#define BATCH_MAX        64
#define PKT_BUF_SIZE     2048

struct epoll_priv {
    int      epfd;
    int      sock_fd;   /* borrowed from turbo_netif.sock_fd */
    int      suspended; /* non-zero → do not call recvmmsg */
};

static struct rtp_packet *epoll_alloc_pkt(struct turbo_netif *tif) {
    (void)tif;
    struct rtp_packet *p = calloc(1, sizeof(*p) + PKT_BUF_SIZE);
    if (!p) return NULL;
    p->data     = (uint8_t *)p + sizeof(*p);
    p->headroom = 0;
    atomic_store(&p->refcount, 1);
    return p;
}

static void epoll_free_pkt(struct turbo_netif *tif, struct rtp_packet *pkt) {
    (void)tif;
    if (!pkt) return;
    if (atomic_fetch_sub(&pkt->refcount, 1) == 1)
        free(pkt);
}

static struct rtp_packet *epoll_clone_pkt(struct turbo_netif *tif,
                                           struct rtp_packet *src) {
    struct rtp_packet *dst = epoll_alloc_pkt(tif);
    if (!dst) return NULL;
    memcpy(dst->data, src->data, src->len);
    dst->len      = src->len;
    dst->src_addr = src->src_addr;
    dst->dst_addr = src->dst_addr;
    dst->alloc_id = src->alloc_id;
    return dst;
}

static int epoll_init(struct turbo_netif *tif, void *cfg) {
    (void)cfg;
    struct epoll_priv *priv = calloc(1, sizeof(*priv));
    if (!priv) return -1;

    priv->sock_fd = tif->sock_fd;
    priv->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (priv->epfd < 0) { free(priv); return -1; }

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = tif->sock_fd,
    };
    if (epoll_ctl(priv->epfd, EPOLL_CTL_ADD, tif->sock_fd, &ev) < 0) {
        close(priv->epfd); free(priv); return -1;
    }
    tif->priv = priv;
    return 0;
}

static void epoll_close(struct turbo_netif *tif) {
    struct epoll_priv *priv = tif->priv;
    if (!priv) return;
    if (priv->epfd >= 0) close(priv->epfd);
    free(priv);
    tif->priv = NULL;
}

static void epoll_suspend(struct turbo_netif *tif) {
    struct epoll_priv *priv = tif->priv;
    if (priv) {
        epoll_ctl(priv->epfd, EPOLL_CTL_DEL, priv->sock_fd, NULL);
        priv->suspended = 1;
    }
}

static void epoll_resume(struct turbo_netif *tif) {
    struct epoll_priv *priv = tif->priv;
    if (!priv || !priv->suspended) return;
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = priv->sock_fd };
    epoll_ctl(priv->epfd, EPOLL_CTL_ADD, priv->sock_fd, &ev);
    priv->suspended = 0;
}

static int epoll_recv_pkts(struct turbo_netif *tif,
                            struct rtp_packet **pkts, uint16_t max) {
    struct epoll_priv *priv = tif->priv;
    if (priv->suspended || max == 0) return 0;

    /* Wait up to 5ms */
    struct epoll_event ev;
    int n = epoll_wait(priv->epfd, &ev, EPOLL_MAX_EVENTS, 5);
    if (n <= 0) return 0;

    /* Batch receive with recvmmsg */
    int batch = max < BATCH_MAX ? max : BATCH_MAX;
    struct mmsghdr msgs[BATCH_MAX];
    struct iovec   iovecs[BATCH_MAX];
    /* We allocate packet objects first, point iovecs at their data buffers */
    struct rtp_packet *pkt_arr[BATCH_MAX];
    int allocated = 0;

    for (int i = 0; i < batch; i++) {
        pkt_arr[i] = epoll_alloc_pkt(tif);
        if (!pkt_arr[i]) { batch = i; break; }
        allocated++;
        iovecs[i].iov_base = pkt_arr[i]->data;
        iovecs[i].iov_len  = PKT_BUF_SIZE;
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov    = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name   = &pkt_arr[i]->src_addr;
        msgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in6);
    }

    int received = recvmmsg(priv->sock_fd, msgs, batch, MSG_DONTWAIT, NULL);
    if (received <= 0) {
        for (int i = 0; i < allocated; i++)
            epoll_free_pkt(tif, pkt_arr[i]);
        return 0;
    }

    for (int i = 0; i < received; i++) {
        pkt_arr[i]->len = (uint16_t)msgs[i].msg_len;
        pkts[i] = pkt_arr[i];
    }
    /* Free any over-allocated */
    for (int i = received; i < allocated; i++)
        epoll_free_pkt(tif, pkt_arr[i]);

    tif->rx_packets += received;
    return received;
}

static int epoll_send_burst(struct turbo_netif *tif,
                             struct rtp_packet **pkts, uint16_t count) {
    struct epoll_priv *priv = tif->priv;
    int sent = 0;

    struct mmsghdr msgs[BATCH_MAX];
    struct iovec   iovecs[BATCH_MAX];

    int batch = count < BATCH_MAX ? count : BATCH_MAX;
    for (int i = 0; i < batch; i++) {
        iovecs[i].iov_base = pkts[i]->data;
        iovecs[i].iov_len  = pkts[i]->len;
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov     = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen  = 1;
        msgs[i].msg_hdr.msg_name    = &pkts[i]->dst_addr;
        msgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in6);
    }

    sent = sendmmsg(priv->sock_fd, msgs, batch, 0);
    if (sent < 0) sent = 0;
    tif->tx_packets += sent;
    return sent;
}

struct turbo_netif_ops turbo_epoll_ops = {
    .init       = epoll_init,
    .close      = epoll_close,
    .suspend    = epoll_suspend,
    .resume     = epoll_resume,
    .recv_pkts  = epoll_recv_pkts,
    .send_burst = epoll_send_burst,
    .alloc_pkt  = epoll_alloc_pkt,
    .free_pkt   = epoll_free_pkt,
    .clone_pkt  = epoll_clone_pkt,
};
