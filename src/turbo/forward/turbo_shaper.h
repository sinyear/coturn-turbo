/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Packet classifier and STUN rate-limiter.
 * Design doc §4.2.
 *
 * Each inbound packet is classified into one of three outcomes:
 *   SHAPER_DROP         — STUN rate-limit exceeded; drop silently.
 *   SHAPER_STUN_QUEUE   — STUN/control packet; forward to libevent ring.
 *   SHAPER_MEDIA_QUEUE  — Media packet (ChannelData / RTP); fast path.
 */

#ifndef TURBO_SHAPER_H
#define TURBO_SHAPER_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/* Classification results */
#define SHAPER_DROP        0
#define SHAPER_STUN_QUEUE  1
#define SHAPER_MEDIA_QUEUE 2

/* Token-bucket defaults */
#define TURBO_STUN_PPS_DEFAULT   2000   /* sustained rate (packets/sec) */
#define TURBO_STUN_BURST_DEFAULT 4000   /* burst capacity */

struct turbo_pkt_shaper {
    uint64_t stun_pps_limit;       /* sustained rate limit */
    uint64_t stun_burst_limit;     /* burst capacity */

    /* Token bucket state — accessed only from worker thread (no lock needed) */
    int64_t  tokens;               /* current token count */
    uint64_t last_refill_ns;       /* nanosecond timestamp of last refill */

    /* Counters (atomic for admin-API reads from other threads) */
    _Atomic uint64_t stun_passed_total;
    _Atomic uint64_t stun_dropped_total;
    _Atomic uint64_t media_passed_total;
};

/* Initialise shaper with default limits */
void turbo_shaper_init(struct turbo_pkt_shaper *s);

/*
 * Classify one packet.
 * buf/len: raw UDP payload.
 * Returns SHAPER_DROP, SHAPER_STUN_QUEUE, or SHAPER_MEDIA_QUEUE.
 */
int turbo_shaper_classify(struct turbo_pkt_shaper *s,
                           const uint8_t *buf, size_t len);

#endif /* TURBO_SHAPER_H */
