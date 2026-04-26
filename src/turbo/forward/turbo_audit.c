/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Audit hook implementation.
 * Design doc §9.2.
 *
 * Ring buffer:  SPSC (single producer: worker thread;
 *                     single consumer: flush thread).
 * Flush thread: wakes every 10 s, drains ring, sends JSON lines
 *               to a Unix datagram socket.  Silently skips if the
 *               socket is unreachable.
 */

#include "turbo_audit.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stdatomic.h>

/* ------------------------------------------------------------------ */
/* Global instance                                                      */
/* ------------------------------------------------------------------ */

struct turbo_audit g_turbo_audit;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static uint32_t next_power_of_two(uint32_t v) {
    if (v == 0) return 64;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Flush thread                                                         */
/* ------------------------------------------------------------------ */

static const char *event_type_str(uint8_t type) {
    switch (type) {
    case TURBO_AUDIT_ALLOC_CREATE:  return "alloc_create";
    case TURBO_AUDIT_ALLOC_DESTROY: return "alloc_destroy";
    case TURBO_AUDIT_MEDIA_FWD:     return "media_fwd";
    case TURBO_AUDIT_ROOM_JOIN:     return "room_join";
    case TURBO_AUDIT_ROOM_LEAVE:    return "room_leave";
    default:                        return "unknown";
    }
}

/*
 * Open (or reopen) a Unix datagram socket connected to the configured path.
 * Returns fd ≥ 0 on success, -1 if path is empty or connect fails.
 */
static int open_sink(const char *path) {
    if (!path || path[0] == '\0') return -1;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Format one event as a JSON line into buf (NUL-terminated).
 * Returns number of bytes written (not including NUL), or -1 on truncation. */
static int format_event(const struct turbo_audit_event *ev,
                         char *buf, size_t bufsz) {
    if (ev->room_id[0] != '\0') {
        return snprintf(buf, bufsz,
            "{\"type\":\"%s\",\"alloc_id\":%u,\"room_id\":\"%s\","
            "\"member_id\":\"%s\",\"bytes\":%llu,\"ts_ns\":%llu}\n",
            event_type_str(ev->type), ev->alloc_id,
            ev->room_id, ev->member_id,
            (unsigned long long)ev->bytes,
            (unsigned long long)ev->timestamp_ns);
    } else {
        return snprintf(buf, bufsz,
            "{\"type\":\"%s\",\"alloc_id\":%u,\"bytes\":%llu,\"ts_ns\":%llu}\n",
            event_type_str(ev->type), ev->alloc_id,
            (unsigned long long)ev->bytes,
            (unsigned long long)ev->timestamp_ns);
    }
}

static void *flush_thread_func(void *arg) {
    struct turbo_audit *a = (struct turbo_audit *)arg;
    int sink_fd = open_sink(a->socket_path);

    while (atomic_load_explicit(&a->running, __ATOMIC_ACQUIRE)) {
        /* Sleep 10 seconds between flushes */
        struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        if (!atomic_load_explicit(&a->running, __ATOMIC_ACQUIRE))
            break;

        /* Drain everything available */
        uint32_t head = atomic_load_explicit(&a->head, __ATOMIC_ACQUIRE);
        uint32_t tail = atomic_load_explicit(&a->tail, __ATOMIC_RELAXED);

        /* Try to (re)open socket if not connected */
        if (sink_fd < 0)
            sink_fd = open_sink(a->socket_path);

        while (tail != head) {
            const struct turbo_audit_event *ev =
                &a->ring[tail & a->mask];

            if (sink_fd >= 0) {
                char line[512];
                int  n = format_event(ev, line, sizeof(line));
                if (n > 0 && n < (int)sizeof(line)) {
                    ssize_t sent = send(sink_fd, line, (size_t)n, MSG_DONTWAIT);
                    if (sent < 0 && (errno == ECONNREFUSED || errno == ENOENT)) {
                        /* Consumer went away — close and retry next round */
                        close(sink_fd);
                        sink_fd = -1;
                    }
                }
            }

            tail++;
            atomic_store_explicit(&a->tail, tail, __ATOMIC_RELEASE);
        }
    }

    if (sink_fd >= 0)
        close(sink_fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int turbo_audit_init(struct turbo_audit *a, uint32_t ring_size,
                      const char *socket_path) {
    memset(a, 0, sizeof(*a));

    uint32_t sz = next_power_of_two(ring_size > 0 ? ring_size
                                                   : TURBO_AUDIT_RING_DEFAULT);
    a->ring = calloc(sz, sizeof(struct turbo_audit_event));
    if (!a->ring) return -1;

    a->mask = sz - 1;
    atomic_init(&a->head,    0);
    atomic_init(&a->tail,    0);
    atomic_init(&a->dropped, 0);
    atomic_init(&a->running, 1);

    if (socket_path && socket_path[0])
        strncpy(a->socket_path, socket_path, sizeof(a->socket_path) - 1);

    if (pthread_create(&a->flush_thread, NULL, flush_thread_func, a) != 0) {
        free(a->ring);
        a->ring = NULL;
        return -1;
    }
    return 0;
}

void turbo_audit_record(struct turbo_audit *a,
                         const struct turbo_audit_event *ev) {
    if (!a || !a->ring) return;

    uint32_t head = atomic_load_explicit(&a->head, __ATOMIC_RELAXED);
    uint32_t tail = atomic_load_explicit(&a->tail, __ATOMIC_ACQUIRE);

    if ((head - tail) >= (a->mask + 1)) {
        /* Ring full — drop and count */
        atomic_fetch_add_explicit(&a->dropped, 1, __ATOMIC_RELAXED);
        return;
    }

    a->ring[head & a->mask] = *ev;
    atomic_store_explicit(&a->head, head + 1, __ATOMIC_RELEASE);
}

void turbo_audit_media(struct turbo_audit *a, uint32_t alloc_id,
                        uint64_t bytes) {
    if (!a || !a->ring) return;
    struct turbo_audit_event ev = {
        .type         = TURBO_AUDIT_MEDIA_FWD,
        .alloc_id     = alloc_id,
        .bytes        = bytes,
        .timestamp_ns = now_ns(),
    };
    turbo_audit_record(a, &ev);
}

void turbo_audit_deinit(struct turbo_audit *a) {
    if (!a || !a->ring) return;
    atomic_store_explicit(&a->running, 0, __ATOMIC_RELEASE);
    pthread_join(a->flush_thread, NULL);
    free(a->ring);
    a->ring = NULL;
}
