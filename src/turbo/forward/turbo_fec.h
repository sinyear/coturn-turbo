#ifndef TURBO_FEC_H
#define TURBO_FEC_H

#include <stdint.h>
#include <stddef.h>

/**
 * Forward Error Correction (FEC) module.
 *
 * Uses Reed-Solomon encoding to recover from packet loss.
 * For every K source packets, generate M repair packets.
 * Can recover from up to M lost packets in each group.
 *
 * This is an optional feature for lossy networks.
 * Disabled by default; planned as an optional feature for lossy networks.
 */

/* FEC context */
struct turbo_fec_ctx {
    uint32_t k;  /* Number of source packets per group */
    uint32_t m;  /* Number of repair packets per group */
    uint8_t *repair_buf;  /* Pre-allocated repair buffer */
    size_t max_pkt_size;   /* Maximum packet size */
};

/**
 * Initialize FEC context
 * @param ctx FEC context
 * @param k Number of source packets
 * @param m Number of repair packets
 * @param max_pkt_size Maximum packet size
 * @return 0 on success, negative error code on failure
 */
int turbo_fec_init(struct turbo_fec_ctx *ctx, uint32_t k, uint32_t m, size_t max_pkt_size);

/**
 * Cleanup FEC context
 * @param ctx FEC context
 */
void turbo_fec_cleanup(struct turbo_fec_ctx *ctx);

/**
 * Generate repair packets from source packets
 * @param ctx FEC context
 * @param src_pkts Array of source packet pointers
 * @param src_lens Array of source packet lengths
 * @param repair_pkts Output array for repair packets
 * @return Number of repair packets generated
 */
int turbo_fec_encode(struct turbo_fec_ctx *ctx,
                     const uint8_t **src_pkts,
                     const size_t *src_lens,
                     uint8_t **repair_pkts);

/**
 * Attempt to recover lost packets using repair packets
 * @param ctx FEC context
 * @param pkts Array of received packets (NULL for missing)
 * @param lens Array of packet lengths
 * @param k Total packets in group
 * @param m Number of repair packets available
 * @return Number of recovered packets
 */
int turbo_fec_decode(struct turbo_fec_ctx *ctx,
                     uint8_t **pkts,
                     size_t *lens,
                     uint32_t k,
                     uint32_t m);

#endif /* TURBO_FEC_H */
