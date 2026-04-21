#include "turbo_mempool.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

/* ============================================================
 * Generic malloc-based pool implementation (always available)
 * ============================================================ */

struct generic_buf {
    struct generic_buf *next;
    size_t size;
};

struct generic_pool_priv {
    struct generic_buf *free_list;
    pthread_mutex_t lock;
};

__attribute__((unused))
static struct turbo_mempool* generic_pool_create(uint32_t nb_bufs, size_t buf_size) {
    struct turbo_mempool *pool = (struct turbo_mempool *)calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    struct generic_pool_priv *priv = (struct generic_pool_priv *)calloc(1, sizeof(*priv));
    if (!priv) {
        free(pool);
        return NULL;
    }

    pool->type = TURBO_POOL_GENERIC;
    pool->total = nb_bufs;
    pool->free = nb_bufs;
    pool->buf_size = buf_size;
    pool->priv = priv;

    /* Pre-allocate all buffers */
    for (uint32_t i = 0; i < nb_bufs; i++) {
        struct generic_buf *buf = (struct generic_buf *)calloc(1, sizeof(*buf) + buf_size);
        if (!buf) {
            /* Partial pool is still usable */
            pool->free = i;
            break;
        }
        buf->size = buf_size;
        buf->next = priv->free_list;
        priv->free_list = buf;
    }

    pthread_mutex_init(&priv->lock, NULL);
    return pool;
}

static void* generic_pool_alloc(struct turbo_mempool *pool, size_t size) {
    if (!pool || size > pool->buf_size) {
        return NULL;
    }

    struct generic_pool_priv *priv = (struct generic_pool_priv *)pool->priv;

    pthread_mutex_lock(&priv->lock);
    struct generic_buf *buf = priv->free_list;
    if (buf) {
        priv->free_list = buf->next;
        pool->used++;
        pool->free--;
    }
    pthread_mutex_unlock(&priv->lock);

    return buf ? (void *)(buf + 1) : NULL;
}

static void generic_pool_free_buf(struct turbo_mempool *pool, void *buf) {
    if (!pool || !buf) {
        return;
    }

    struct generic_pool_priv *priv = (struct generic_pool_priv *)pool->priv;
    struct generic_buf *gbuf = (struct generic_buf *)buf - 1;

    pthread_mutex_lock(&priv->lock);
    gbuf->next = priv->free_list;
    priv->free_list = gbuf;
    pool->used--;
    pool->free++;
    pthread_mutex_unlock(&priv->lock);
}

static void generic_pool_destroy(struct turbo_mempool *pool) {
    if (!pool) {
        return;
    }

    struct generic_pool_priv *priv = (struct generic_pool_priv *)pool->priv;
    if (priv) {
        pthread_mutex_lock(&priv->lock);
        struct generic_buf *buf = priv->free_list;
        while (buf) {
            struct generic_buf *next = buf->next;
            free(buf);
            buf = next;
        }
        pthread_mutex_unlock(&priv->lock);
        pthread_mutex_destroy(&priv->lock);
        free(priv);
    }
    free(pool);
}

/* ============================================================
 * DPDK backend (conditional)
 * ============================================================ */

#ifdef TURN_USE_DPDK

#include <rte_mempool.h>
#include <rte_mbuf.h>

struct dpdk_pool_priv {
    struct rte_mempool *mbuf_pool;
    size_t buf_size;
};

struct turbo_mempool* turbo_mempool_create_dpdk(const char *name,
                                                 uint32_t nb_mbufs,
                                                 size_t buf_size,
                                                 unsigned int socket_id) {
    struct turbo_mempool *pool = (struct turbo_mempool *)calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    struct dpdk_pool_priv *priv = (struct dpdk_pool_priv *)calloc(1, sizeof(*priv));
    if (!priv) {
        free(pool);
        return NULL;
    }

    /* Create DPDK mbuf pool */
    priv->mbuf_pool = rte_pktmbuf_pool_create(name, nb_mbufs, 256, 0,
                                               (uint16_t)(buf_size > 2048 ? 2048 : buf_size),
                                               socket_id);
    if (!priv->mbuf_pool) {
        free(priv);
        free(pool);
        return NULL;
    }

    pool->type = TURBO_POOL_DPDK;
    pool->total = nb_mbufs;
    pool->free = nb_mbufs;
    pool->buf_size = buf_size;
    pool->priv = priv;

    return pool;
}

static void* dpdk_pool_alloc(struct turbo_mempool *pool, size_t size) {
    if (!pool || !pool->priv) {
        return NULL;
    }

    struct dpdk_pool_priv *priv = (struct dpdk_pool_priv *)pool->priv;
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(priv->mbuf_pool);
    if (!mbuf) {
        return NULL;
    }

    pool->used++;
    pool->free--;
    return mbuf;
}

static void dpdk_pool_free_buf(struct turbo_mempool *pool, void *buf) {
    if (!pool || !buf) {
        return;
    }

    struct dpdk_pool_priv *priv = (struct dpdk_pool_priv *)pool->priv;
    rte_pktmbuf_free((struct rte_mbuf *)buf);
    pool->used--;
    pool->free++;
}

static void dpdk_pool_destroy(struct turbo_mempool *pool) {
    if (!pool) {
        return;
    }

    struct dpdk_pool_priv *priv = (struct dpdk_pool_priv *)pool->priv;
    if (priv && priv->mbuf_pool) {
        rte_mempool_free(priv->mbuf_pool);
    }
    free(priv);
    free(pool);
}

#else /* !TURN_USE_DPDK */

struct turbo_mempool* turbo_mempool_create_dpdk(const char *name,
                                                 uint32_t nb_mbufs,
                                                 size_t buf_size,
                                                 unsigned int socket_id) {
    (void)name; (void)nb_mbufs; (void)buf_size; (void)socket_id;
    return NULL;
}

#endif /* TURN_USE_DPDK */

/* ============================================================
 * AF_XDP backend (conditional)
 * ============================================================ */

#ifdef TURN_USE_AFXDP

#include <xdp/xsk.h>

struct afxdp_pool_priv {
    struct xsk_umem *umem;
    void *umem_area;
    uint32_t num_frames;
    uint32_t frame_size;
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
};

struct turbo_mempool* turbo_mempool_create_afxdp(uint32_t num_frames, size_t frame_size) {
    struct turbo_mempool *pool = (struct turbo_mempool *)calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    struct afxdp_pool_priv *priv = (struct afxdp_pool_priv *)calloc(1, sizeof(*priv));
    if (!priv) {
        free(pool);
        return NULL;
    }

    priv->num_frames = num_frames;
    priv->frame_size = (uint32_t)frame_size;

    struct xsk_umem_config umem_cfg = {0};
    umem_cfg.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    umem_cfg.frame_size = frame_size;
    umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;

    /* Allocate UMEM area */
    priv->umem_area = aligned_alloc(sysconf(_SC_PAGESIZE), num_frames * frame_size);
    if (!priv->umem_area) {
        free(priv);
        free(pool);
        return NULL;
    }

    /* Create UMEM */
    int ret = xsk_umem__create(&priv->umem, priv->umem_area,
                               num_frames * frame_size,
                               &priv->fq, &priv->cq, &umem_cfg);
    if (ret) {
        free(priv->umem_area);
        free(priv);
        free(pool);
        return NULL;
    }

    pool->type = TURBO_POOL_AFXDP;
    pool->total = num_frames;
    pool->free = num_frames;
    pool->buf_size = frame_size;
    pool->priv = priv;

    return pool;
}

static void* afxdp_pool_alloc(struct turbo_mempool *pool, size_t size) {
    (void)size;
    if (!pool || !pool->priv) {
        return NULL;
    }

    struct afxdp_pool_priv *priv = (struct afxdp_pool_priv *)pool->priv;
    uint32_t idx;

    if (xsk_ring_prod__reserve(&priv->fq, 1, &idx) != 1) {
        return NULL;
    }

    uint64_t addr = *xsk_ring_prod__fill_addr(&priv->fq, idx);
    xsk_ring_prod__submit(&priv->fq, 1);

    pool->used++;
    pool->free--;
    return xsk_umem__get_data(priv->umem_area, addr);
}

static void afxdp_pool_free_buf(struct turbo_mempool *pool, void *buf) {
    /* AF_XDP frames are managed by UMEM ring descriptors.
     * Return frame to fill queue for reuse. */
    if (!pool || !pool->priv) {
        return;
    }

    struct afxdp_pool_priv *priv = (struct afxdp_pool_priv *)pool->priv;
    uint64_t addr = (uint64_t)((char *)buf - (char *)priv->umem_area);
    uint32_t idx;

    if (xsk_ring_prod__reserve(&priv->fq, 1, &idx) == 1) {
        *xsk_ring_prod__fill_addr(&priv->fq, idx) = addr;
        xsk_ring_prod__submit(&priv->fq, 1);
    }

    pool->used--;
    pool->free++;
}

static void afxdp_pool_destroy(struct turbo_mempool *pool) {
    if (!pool) {
        return;
    }

    struct afxdp_pool_priv *priv = (struct afxdp_pool_priv *)pool->priv;
    if (priv) {
        if (priv->umem) {
            xsk_umem__delete(priv->umem);
        }
        free(priv->umem_area);
        free(priv);
    }
    free(pool);
}

#else /* !TURN_USE_AFXDP */

struct turbo_mempool* turbo_mempool_create_afxdp(uint32_t num_frames, size_t frame_size) {
    (void)num_frames; (void)frame_size;
    return NULL;
}

#endif /* TURN_USE_AFXDP */

/* ============================================================
 * Public API (backend-agnostic)
 * ============================================================ */

void* turbo_mempool_alloc(struct turbo_mempool *pool, size_t size) {
    if (!pool) {
        return NULL;
    }

    switch (pool->type) {
#ifdef TURN_USE_DPDK
    case TURBO_POOL_DPDK:
        return dpdk_pool_alloc(pool, size);
#endif
#ifdef TURN_USE_AFXDP
    case TURBO_POOL_AFXDP:
        return afxdp_pool_alloc(pool, size);
#endif
    case TURBO_POOL_GENERIC:
        return generic_pool_alloc(pool, size);
    default:
        return NULL;
    }
}

void turbo_mempool_free(struct turbo_mempool *pool, void *buf) {
    if (!pool || !buf) {
        return;
    }

    switch (pool->type) {
#ifdef TURN_USE_DPDK
    case TURBO_POOL_DPDK:
        dpdk_pool_free_buf(pool, buf);
        return;
#endif
#ifdef TURN_USE_AFXDP
    case TURBO_POOL_AFXDP:
        afxdp_pool_free_buf(pool, buf);
        return;
#endif
    case TURBO_POOL_GENERIC:
        generic_pool_free_buf(pool, buf);
        return;
    default:
        return;
    }
}

void turbo_mempool_stats(struct turbo_mempool *pool,
                         uint32_t *total, uint32_t *used, uint32_t *free_count) {
    if (!pool) {
        return;
    }
    if (total) *total = pool->total;
    if (used) *used = pool->used;
    if (free_count) *free_count = pool->free;
}

void turbo_mempool_destroy(struct turbo_mempool *pool) {
    if (!pool) {
        return;
    }

    switch (pool->type) {
#ifdef TURN_USE_DPDK
    case TURBO_POOL_DPDK:
        dpdk_pool_destroy(pool);
        return;
#endif
#ifdef TURN_USE_AFXDP
    case TURBO_POOL_AFXDP:
        afxdp_pool_destroy(pool);
        return;
#endif
    case TURBO_POOL_GENERIC:
        generic_pool_destroy(pool);
        return;
    default:
        return;
    }
}
