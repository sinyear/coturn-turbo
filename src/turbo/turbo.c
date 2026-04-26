/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Turbo top-level init/deinit and packet dispatch.
 * Design doc §6.4 (startup sequence) and §T3.5 (process_packet).
 */

#ifdef TURBO_FEATURES

#include "turbo.h"
#include "netif/turbo_netif.h"
#include "forward/turbo_fastpath.h"
#include "forward/turbo_shaper.h"
#include "forward/turbo_switch.h"
#include "common/turbo_ring.h"
#include "room/turbo_room.h"
#include "room/turbo_room_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

struct turbo_netif      g_turbo_netif;
struct turbo_fastpath   g_turbo_fastpath;
struct turbo_pkt_shaper g_turbo_shaper;
struct turbo_ring       g_ctrl_ring;   /* Worker → libevent STUN queue */

/* Active room provider — set by turbo_rooms_configure() before turbo_init() */
struct turbo_room_provider_ops *g_turbo_room_provider = NULL;

/* Pending provider config (set by turbo_rooms_configure before init) */
static char  g_rooms_provider_name[32]  = {0};
static char  g_rooms_secret[256]        = {0};
static uint64_t g_rooms_max_expiry      = 0;

void *turbo_shared_relay_socket = NULL;  /* ioa_socket_handle (opaque) */
static int turbo_shared_relay_fd = -1;

static pthread_t g_worker_thread;
static int       g_worker_started = 0;

/* ------------------------------------------------------------------ */
/* SIGUSR1: async-safe flag, executed in worker loop                    */
/* ------------------------------------------------------------------ */

_Atomic int turbo_degrade_requested = 0;

static void handle_sigusr1(int sig) {
    (void)sig;
    atomic_store(&turbo_degrade_requested, 1);
}

/* ------------------------------------------------------------------ */
/* Shared relay socket helpers                                          */
/* ------------------------------------------------------------------ */

void *turbo_get_shared_relay_socket(void) {
    return turbo_shared_relay_socket;
}

int turbo_get_shared_relay_fd(void) {
    return turbo_shared_relay_fd;
}

/* ------------------------------------------------------------------ */
/* Init / deinit                                                        */
/* ------------------------------------------------------------------ */

int turbo_init(void) {
    /* 1. Create shared relay socket bound to TURBO_RELAY_PORT */
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("turbo: socket");
        return -1;
    }

    int v6only = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(TURBO_RELAY_PORT);
    addr.sin6_addr   = in6addr_any;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("turbo: bind relay socket");
        close(fd);
        return -1;
    }
    turbo_shared_relay_fd = fd;

    /* 2. Initialise fastpath tables */
    if (turbo_fastpath_init(&g_turbo_fastpath) != 0) {
        fprintf(stderr, "turbo: fastpath init failed\n");
        close(fd);
        return -1;
    }

    /* 3. Initialise shaper */
    turbo_shaper_init(&g_turbo_shaper);

    /* 4. Initialise Worker↔libevent control ring */
    if (turbo_ring_init(&g_ctrl_ring, 1024) != 0) {
        fprintf(stderr, "turbo: ring init failed\n");
        close(fd);
        return -1;
    }

    /* 5. Select backend type from compile-time flags */
    int backend_type;
#if defined(TURBO_AFXDP)
    backend_type = TURBO_BACKEND_AF_XDP;
#elif defined(TURBO_IOURING)
    backend_type = TURBO_BACKEND_IO_URING;
#else
    backend_type = TURBO_BACKEND_EPOLL;
#endif

    /* 6. Initialise network backend (auto-fallbacks toward epoll) */
    if (turbo_netif_init(&g_turbo_netif, backend_type, fd) != 0) {
        fprintf(stderr, "turbo: netif init failed\n");
        turbo_ring_destroy(&g_ctrl_ring);
        close(fd);
        return -1;
    }

    /* 7. Start worker thread */
    if (pthread_create(&g_worker_thread, NULL,
                       turbo_worker_loop, &g_turbo_netif) != 0) {
        perror("turbo: pthread_create worker");
        turbo_netif_close(&g_turbo_netif);
        turbo_ring_destroy(&g_ctrl_ring);
        close(fd);
        return -1;
    }
    g_worker_started = 1;

    /* 8. Register SIGUSR1 for manual degrade */
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* 9. Initialise room subsystem */
    turbo_room_init(50);

    /* 10. Activate room provider if configured */
    if (g_rooms_provider_name[0] != '\0') {
        g_turbo_room_provider = turbo_provider_get(g_rooms_provider_name);
        if (g_turbo_room_provider) {
            if (g_turbo_room_provider->init(NULL) != 0) {
                fprintf(stderr, "turbo: room provider '%s' init failed\n",
                        g_rooms_provider_name);
                g_turbo_room_provider = NULL;
            } else {
                fprintf(stderr, "turbo: room provider '%s' active\n",
                        g_rooms_provider_name);
            }
        } else {
            fprintf(stderr, "turbo: unknown room provider '%s'\n",
                    g_rooms_provider_name);
        }
    }

    fprintf(stderr, "turbo: initialized (relay fd=%d)\n", fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Room provider pre-init configuration                                 */
/* ------------------------------------------------------------------ */

void turbo_rooms_configure(const char *provider_name, const char *secret,
                             uint64_t max_expiry) {
    if (provider_name && provider_name[0])
        strncpy(g_rooms_provider_name, provider_name, sizeof(g_rooms_provider_name) - 1);
    if (secret && secret[0])
        strncpy(g_rooms_secret, secret, sizeof(g_rooms_secret) - 1);
    g_rooms_max_expiry = max_expiry;

    /* Pass secret to hmac provider ahead of init */
    if (g_rooms_secret[0]) {
        turbo_token_hmac_set_secret(g_rooms_secret, strlen(g_rooms_secret));
    }
    if (max_expiry > 0) {
        turbo_token_hmac_set_max_expiry(max_expiry);
    }
}

void turbo_deinit(void) {
    /* Signal worker to stop and join */
    atomic_store(&g_turbo_netif.running, 0);
    if (g_worker_started) {
        pthread_join(g_worker_thread, NULL);
        g_worker_started = 0;
    }

    turbo_netif_close(&g_turbo_netif);
    turbo_fastpath_deinit(&g_turbo_fastpath);
    turbo_ring_destroy(&g_ctrl_ring);
    turbo_room_deinit();

    if (g_turbo_room_provider) {
        g_turbo_room_provider->deinit();
        g_turbo_room_provider = NULL;
    }

    if (turbo_shared_relay_fd >= 0) {
        close(turbo_shared_relay_fd);
        turbo_shared_relay_fd = -1;
    }
}

/* ------------------------------------------------------------------ */
/* libevent-thread helpers                                              */
/* ------------------------------------------------------------------ */

void turbo_l1_cache_warmup(void *ss, const void *client_addr) {
    (void)ss;
    (void)client_addr;
    /* Superseded by turbo_fastpath_warmup_alloc() which carries the full info. */
}

void turbo_override_relay_port(void *ss, uint16_t port) {
    (void)ss;
    (void)port;
    /* Relay port override is implemented directly in ns_turn_server.c
     * where the allocation struct is accessible. */
}

void turbo_fastpath_warmup_alloc(uint32_t alloc_id,
                                  const void *client_addr,
                                  const void *peer_addr,
                                  const char *room_id,
                                  uint64_t expiry) {
    turbo_fastpath_warmup(&g_turbo_fastpath,
                           alloc_id,
                           (const struct sockaddr_in6 *)client_addr,
                           (const struct sockaddr_in6 *)peer_addr,
                           room_id,
                           expiry);
}

/* ------------------------------------------------------------------ */
/* turbo_process_packet — called by worker loop for every RX packet     */
/* ------------------------------------------------------------------ */

/* Override the weak default in turbo_netif.c */
void turbo_process_packet(struct turbo_netif *tif, struct rtp_packet *pkt) {
    /* 1. Classify via shaper */
    int cls = turbo_shaper_classify(&g_turbo_shaper, pkt->data, pkt->len);

    if (cls == SHAPER_DROP) {
        tif->ops->free_pkt(tif, pkt);
        return;
    }

    if (cls == SHAPER_STUN_QUEUE) {
        /* Hand off to libevent thread via lock-free ring */
        if (!turbo_ring_push(&g_ctrl_ring, pkt)) {
            /* Ring full — drop with accounting */
            tif->dropped_packets++;
            tif->ops->free_pkt(tif, pkt);
        }
        return;
    }

    /* 2. Three-table fastpath lookup */
    uint32_t alloc_id = turbo_fastpath_lookup(&g_turbo_fastpath,
                                               pkt->data, pkt->len,
                                               &pkt->src_addr);
    if (!alloc_id) {
        /* Fastpath miss — forward to libevent for full TURN processing */
        if (!turbo_ring_push(&g_ctrl_ring, pkt)) {
            tif->dropped_packets++;
            tif->ops->free_pkt(tif, pkt);
        }
        return;
    }

    pkt->alloc_id = alloc_id;

    /* 3. Read allocation snapshot via seqlock */
    struct turbo_alloc_snapshot snap;
    if (turbo_fastpath_read_alloc(&g_turbo_fastpath, alloc_id, &snap) != 0) {
        tif->ops->free_pkt(tif, pkt);
        return;
    }

    /* 4. Forward: unicast or room broadcast */
    if (snap.room_id[0] != '\0') {
        turbo_room_broadcast(tif, snap.room_id, alloc_id, pkt);
        tif->ops->free_pkt(tif, pkt);
    } else {
        /* Unicast: rewrite destination and send */
        pkt->dst_addr = snap.peer_addr;
        struct rtp_packet *clone = tif->ops->clone_pkt(tif, pkt);
        if (clone) {
            tif->ops->send_burst(tif, &clone, 1);
            tif->ops->free_pkt(tif, clone);
        }
        tif->ops->free_pkt(tif, pkt);
    }
}

/* ------------------------------------------------------------------ */
/* Allocation teardown — called from ns_turn_server.c on session close  */
/* ------------------------------------------------------------------ */

void turbo_alloc_teardown(uint32_t alloc_id,
                           const char *room_id,
                           const char *member_id) {
    /* Remove fastpath entries */
    turbo_fastpath_remove(&g_turbo_fastpath, alloc_id);

    /* Remove from room (handles NULL / empty strings safely) */
    if (room_id && room_id[0] != '\0' && member_id && member_id[0] != '\0')
        turbo_room_remove_member(room_id, member_id);
    else if (alloc_id != 0)
        turbo_room_remove_alloc(alloc_id);  /* fallback: scan all rooms */
}

#endif /* TURBO_FEATURES */
