#include "turbo_fec.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Simple XOR-based FEC (single parity).
 *
 * For each group of K source packets, generate M repair packets
 * by XORing all source packets together. This can recover from
 * a single lost packet per group.
 *
 * For more robust FEC (Reed-Solomon), replace with gf-complete library.
 */

int turbo_fec_init(struct turbo_fec_ctx *ctx, uint32_t k, uint32_t m, size_t max_pkt_size) {
    if (!ctx || k == 0 || m == 0 || max_pkt_size == 0) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->k = k;
    ctx->m = m;
    ctx->max_pkt_size = max_pkt_size;

    /* Allocate repair buffer: m packets * max_pkt_size */
    ctx->repair_buf = (uint8_t *)malloc(m * max_pkt_size);
    if (!ctx->repair_buf) {
        return -ENOMEM;
    }

    return 0;
}

void turbo_fec_cleanup(struct turbo_fec_ctx *ctx) {
    if (!ctx) {
        return;
    }

    free(ctx->repair_buf);
    ctx->repair_buf = NULL;
}

/*
 * Generate XOR parity packets.
 * For simplicity, each repair packet is XOR of all source packets.
 */
int turbo_fec_encode(struct turbo_fec_ctx *ctx,
                     const uint8_t **src_pkts,
                     const size_t *src_lens,
                     uint8_t **repair_pkts) {
    if (!ctx || !src_pkts || !repair_pkts || ctx->m == 0) {
        return 0;
    }

    /* Find max length among source packets */
    size_t max_len = 0;
    for (uint32_t i = 0; i < ctx->k; i++) {
        if (src_pkts[i] && src_lens[i] > max_len) {
            max_len = src_lens[i];
        }
    }

    if (max_len == 0) {
        return 0;
    }

    /* Generate each repair packet */
    for (uint32_t r = 0; r < ctx->m; r++) {
        uint8_t *repair = repair_pkts[r];
        memset(repair, 0, max_len);

        /* XOR all source packets together */
        for (uint32_t i = 0; i < ctx->k; i++) {
            if (src_pkts[i] && src_lens[i] > 0) {
                for (size_t j = 0; j < src_lens[i]; j++) {
                    repair[j] ^= src_pkts[i][j];
                }
            }
        }
    }

    return (int)ctx->m;
}

/*
 * Recover lost packets using XOR parity.
 * Only works for single packet loss (XOR limitation).
 */
int turbo_fec_decode(struct turbo_fec_ctx *ctx,
                     uint8_t **pkts,
                     size_t *lens,
                     uint32_t k,
                     uint32_t m) {
    if (!ctx || !pkts || !lens || m == 0) {
        return 0;
    }

    (void)k; /* Suppress unused warning */

    /* Count missing packets */
    int missing = 0;
    int missing_idx = -1;

    for (uint32_t i = 0; i < ctx->k; i++) {
        if (!pkts[i]) {
            missing++;
            missing_idx = (int)i;
        }
    }

    /* XOR can only recover single loss */
    if (missing != 1 || missing_idx < 0) {
        return 0;
    }

    /* Find max length among available packets */
    size_t max_len = 0;
    for (uint32_t i = 0; i < ctx->k + m; i++) {
        if (pkts[i] && lens[i] > max_len) {
            max_len = lens[i];
        }
    }

    if (max_len == 0) {
        return 0;
    }

    /* Recover missing packet by XORing all available packets */
    uint8_t *recovered = (uint8_t *)malloc(max_len);
    if (!recovered) {
        return 0;
    }

    memset(recovered, 0, max_len);

    for (uint32_t i = 0; i < ctx->k + m; i++) {
        if (pkts[i]) {
            for (size_t j = 0; j < lens[i]; j++) {
                recovered[j] ^= pkts[i][j];
            }
        }
    }

    pkts[missing_idx] = recovered;
    lens[missing_idx] = max_len;

    return 1;
}
