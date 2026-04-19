#ifndef TURBO_MEMPOOL_H
#define TURBO_MEMPOOL_H

#include <stdint.h>
#include <stddef.h>

/**
 * Unified memory pool abstraction for DPDK and AF_XDP backends.
 *
 * DPDK: wraps rte_mempool for mbuf allocation
 * AF_XDP: manages UMEM frame allocation with reference counting
 */

typedef enum {
    TURBO_POOL_DPDK,
    TURBO_POOL_AFXDP,
    TURBO_POOL_GENERIC  /* malloc-based fallback */
} turbo_pool_type_t;

struct turbo_mempool {
    turbo_pool_type_t type;
    void *priv;       /* Backend-specific: rte_mempool* or umem handle */
    uint32_t total;   /* Total buffers */
    uint32_t used;    /* Currently allocated */
    uint32_t free;    /* Available */
    size_t buf_size;  /* Buffer size per element */
};

/**
 * Create DPDK mbuf pool
 * @param name Pool name
 * @param nb_mbufs Number of mbufs
 * @param buf_size Buffer size
 * @param socket_id NUMA socket ID
 * @return Pool handle or NULL
 */
struct turbo_mempool* turbo_mempool_create_dpdk(const char *name,
                                                 uint32_t nb_mbufs,
                                                 size_t buf_size,
                                                 unsigned int socket_id);

/**
 * Create AF_XDP UMEM pool
 * @param num_frames Number of frames
 * @param frame_size Frame size
 * @return Pool handle or NULL
 */
struct turbo_mempool* turbo_mempool_create_afxdp(uint32_t num_frames,
                                                  size_t frame_size);

/**
 * Create generic malloc-based pool (fallback)
 * @param nb_bufs Number of buffers
 * @param buf_size Buffer size
 * @return Pool handle or NULL
 */
struct turbo_mempool* turbo_mempool_create_generic(uint32_t nb_bufs, size_t buf_size);

/**
 * Allocate a buffer from the pool
 * @param pool Pool handle
 * @param size Requested size (must be <= buf_size)
 * @return Pointer to allocated buffer, NULL on failure
 */
void* turbo_mempool_alloc(struct turbo_mempool *pool, size_t size);

/**
 * Free a buffer back to the pool
 * @param pool Pool handle
 * @param buf Buffer to free
 */
void turbo_mempool_free(struct turbo_mempool *pool, void *buf);

/**
 * Get pool statistics
 * @param pool Pool handle
 * @param total Output: total buffers
 * @param used Output: allocated buffers
 * @param free_count Output: available buffers
 */
void turbo_mempool_stats(struct turbo_mempool *pool,
                         uint32_t *total, uint32_t *used, uint32_t *free_count);

/**
 * Destroy pool and free all resources
 * @param pool Pool handle
 */
void turbo_mempool_destroy(struct turbo_mempool *pool);

#endif /* TURBO_MEMPOOL_H */
