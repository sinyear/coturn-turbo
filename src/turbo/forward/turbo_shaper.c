/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Packet classifier and STUN rate-limiter implementation.
 * Design doc §4.2.
 */

#include "turbo_shaper.h"
#include <time.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Clock helper                                                         */
/* ------------------------------------------------------------------ */

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Packet type detection                                               */
/* ------------------------------------------------------------------ */

/*
 * ChannelData: first 2 bytes in range [0x4000, 0x7FFF]  (RFC 8656 §12)
 * STUN:        top 2 bits of first byte == 00            (RFC 5389 §6)
 * All others treated as media (e.g. raw RTP from peer side).
 */
static inline int packet_is_channel_data(const uint8_t *buf) {
    uint16_t type = ((uint16_t)buf[0] << 8) | buf[1];
    return type >= 0x4000 && type <= 0x7FFF;
}

static inline int packet_is_stun(const uint8_t *buf) {
    return (buf[0] & 0xC0) == 0x00;
}

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

void turbo_shaper_init(struct turbo_pkt_shaper *s) {
    memset(s, 0, sizeof(*s));
    s->stun_pps_limit   = TURBO_STUN_PPS_DEFAULT;
    s->stun_burst_limit = TURBO_STUN_BURST_DEFAULT;
    s->tokens           = (int64_t)TURBO_STUN_BURST_DEFAULT;
    s->last_refill_ns   = now_ns();
}

/* ------------------------------------------------------------------ */
/* Classify                                                             */
/* ------------------------------------------------------------------ */

int turbo_shaper_classify(struct turbo_pkt_shaper *s,
                           const uint8_t *buf, size_t len) {
    if (len < 2) {
        atomic_fetch_add_explicit(&s->stun_dropped_total, 1, __ATOMIC_RELAXED);
        return SHAPER_DROP;
    }

    /* Media packets bypass the rate limiter entirely */
    if (packet_is_channel_data(buf) || !packet_is_stun(buf)) {
        atomic_fetch_add_explicit(&s->media_passed_total, 1, __ATOMIC_RELAXED);
        return SHAPER_MEDIA_QUEUE;
    }

    /* STUN packet — apply token-bucket rate limit */
    uint64_t now = now_ns();
    uint64_t elapsed_ns = now - s->last_refill_ns;
    s->last_refill_ns = now;

    /* Refill tokens: rate * elapsed_seconds */
    int64_t refill = (int64_t)((s->stun_pps_limit * elapsed_ns) / 1000000000ULL);
    s->tokens += refill;
    if (s->tokens > (int64_t)s->stun_burst_limit)
        s->tokens = (int64_t)s->stun_burst_limit;

    if (s->tokens > 0) {
        s->tokens--;
        atomic_fetch_add_explicit(&s->stun_passed_total, 1, __ATOMIC_RELAXED);
        return SHAPER_STUN_QUEUE;
    }

    /* Rate exceeded */
    atomic_fetch_add_explicit(&s->stun_dropped_total, 1, __ATOMIC_RELAXED);
    return SHAPER_DROP;
}
