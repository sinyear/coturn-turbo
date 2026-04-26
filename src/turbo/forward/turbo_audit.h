/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Audit hook — lock-free ring buffer + background flush to Unix socket.
 * Design doc §9.2.
 *
 * Events are pushed non-blocking from the worker thread into a ring buffer.
 * A dedicated flush thread drains the ring every 10 seconds and writes
 * newline-delimited JSON to a Unix datagram socket.
 *
 * On ring overflow the oldest slot is silently overwritten and
 * g_turbo_audit_dropped is incremented (exposed as a Prometheus counter).
 */

#ifndef TURBO_AUDIT_H
#define TURBO_AUDIT_H

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Event types                                                          */
/* ------------------------------------------------------------------ */

#define TURBO_AUDIT_ALLOC_CREATE   1  /* new allocation established */
#define TURBO_AUDIT_ALLOC_DESTROY  2  /* allocation released */
#define TURBO_AUDIT_MEDIA_FWD      3  /* media packet forwarded (sampled) */
#define TURBO_AUDIT_ROOM_JOIN      4  /* member joined a room */
#define TURBO_AUDIT_ROOM_LEAVE     5  /* member left a room */

/* ------------------------------------------------------------------ */
/* Event record (fixed-size for simple ring indexing)                  */
/* ------------------------------------------------------------------ */

struct turbo_audit_event {
    uint8_t  type;            /* TURBO_AUDIT_* constant */
    uint32_t alloc_id;
    char     room_id[64];
    char     member_id[64];
    uint64_t timestamp_ns;    /* CLOCK_MONOTONIC nanoseconds */
    uint64_t bytes;           /* MEDIA_FWD: forwarded bytes; 0 otherwise */
};

/* ------------------------------------------------------------------ */
/* Ring buffer + flush state                                            */
/* ------------------------------------------------------------------ */

#define TURBO_AUDIT_RING_DEFAULT 4096  /* entries; must be power of two */

struct turbo_audit {
    struct turbo_audit_event *ring;
    uint32_t                  mask;       /* ring_size - 1 */
    _Atomic uint32_t          head;       /* producer cursor (worker thread) */
    _Atomic uint32_t          tail;       /* consumer cursor (flush thread) */
    _Atomic uint64_t          dropped;    /* overflow drop count */
    char     socket_path[256];           /* Unix socket path ("" → no flush) */
    pthread_t  flush_thread;
    _Atomic int running;
};

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/*
 * Initialise the audit subsystem.
 * ring_size: ring capacity (clamped to nearest power-of-two ≥ 64).
 *            Pass 0 to use TURBO_AUDIT_RING_DEFAULT.
 * socket_path: path of the consumer's Unix datagram socket, e.g.
 *              "/var/run/turn-audit.sock".  Pass NULL or "" to disable
 *              flushing (events still counted, just not emitted).
 * Returns 0 on success, -1 on error.
 */
int  turbo_audit_init(struct turbo_audit *a, uint32_t ring_size,
                       const char *socket_path);

/*
 * Submit an event.  Non-blocking; silently drops (and increments
 * a->dropped) when the ring is full.
 */
void turbo_audit_record(struct turbo_audit *a,
                         const struct turbo_audit_event *ev);

/* Convenience: record a MEDIA_FWD event. */
void turbo_audit_media(struct turbo_audit *a, uint32_t alloc_id,
                        uint64_t bytes);

/* Stop the flush thread and release resources. */
void turbo_audit_deinit(struct turbo_audit *a);

/* ------------------------------------------------------------------ */
/* Global instance (zero-initialised; active only after turbo_init)    */
/* ------------------------------------------------------------------ */

extern struct turbo_audit g_turbo_audit;

/* Prometheus-visible overflow counter (alias of g_turbo_audit.dropped) */
#define g_turbo_audit_dropped (g_turbo_audit.dropped)

#endif /* TURBO_AUDIT_H */
