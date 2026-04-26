/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * coturn-turbo main entry header.
 * Include this in coturn relay code to access all Turbo APIs.
 */

#ifndef TURBO_H
#define TURBO_H

#ifdef TURBO_FEATURES

#include "netif/turbo_netif.h"
#include "forward/turbo_fastpath.h"
#include "forward/turbo_shaper.h"
#include "common/turbo_ring.h"
#include "room/turbo_room.h"
#include "room/turbo_room_provider.h"

#define TURBO_RELAY_PORT 3478

/* Global instances (initialised by turbo_init()) */
extern struct turbo_netif      g_turbo_netif;
extern struct turbo_fastpath   g_turbo_fastpath;
extern struct turbo_pkt_shaper g_turbo_shaper;
extern struct turbo_ring       g_ctrl_ring;   /* Worker → libevent STUN queue */

/* Active room identity provider (NULL if rooms disabled) */
extern struct turbo_room_provider_ops *g_turbo_room_provider;

/* Shared relay socket used by all allocations in single-port mode */
extern void *turbo_shared_relay_socket;       /* ioa_socket_handle */

/* Atomic flag set by SIGUSR1 handler; checked by worker loop */
extern _Atomic int             turbo_degrade_requested;

int  turbo_init(void);
void turbo_deinit(void);

/* Called by libevent thread after successful Allocate */
void turbo_l1_cache_warmup(void *ss, const void *client_addr);
void turbo_override_relay_port(void *ss, uint16_t port);

/*
 * Full warmup called from ns_turn_server.c with complete allocation info.
 * alloc_id: coturn allocation identifier (nonzero)
 * client_addr: client's source address (key for L1 cache)
 * peer_addr: relay destination address (may be NULL at Allocate time)
 * room_id: empty string or room identifier (when --turbo-rooms)
 * expiry: allocation expiry (UNIX seconds; 0 = no expiry)
 */
void turbo_fastpath_warmup_alloc(uint32_t alloc_id,
                                  const void *client_addr,
                                  const void *peer_addr,
                                  const char *room_id,
                                  uint64_t expiry);

/*
 * Remove fastpath entries and room membership for an allocation.
 * Called from session teardown in ns_turn_server.c.
 */
void turbo_alloc_teardown(uint32_t alloc_id, const char *room_id, const char *member_id);

void *turbo_get_shared_relay_socket(void);
int   turbo_get_shared_relay_fd(void);

/*
 * Configure room subsystem — must be called before turbo_init().
 * provider_name: "static" | "token_hmac" | NULL (disables rooms)
 * secret: shared HMAC secret (only for token_hmac)
 * max_expiry: max token lifetime in seconds (0 = unlimited)
 */
void turbo_rooms_configure(const char *provider_name, const char *secret,
                             uint64_t max_expiry);

#endif /* TURBO_FEATURES */
#endif /* TURBO_H */
