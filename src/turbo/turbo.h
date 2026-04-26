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
#include <time.h>

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

/* Server start time (set in turbo_init; used for uptime reporting) */
extern time_t                  g_turbo_start_time;

int  turbo_init(void);
void turbo_deinit(void);

/* Called by libevent thread after successful Allocate */
void turbo_l1_cache_warmup(void *ss, const void *client_addr);
void turbo_override_relay_port(void *ss, uint16_t port);

/*
 * Acquire a unique alloc_id from the fastpath table (P6: no mod collisions).
 * Returns nonzero on success, 0 if table is full.
 */
uint32_t turbo_alloc_id_acquire(void);

/*
 * Full warmup called from ns_turn_server.c with complete allocation info.
 * alloc_id: obtained from turbo_alloc_id_acquire()
 * client_addr: client's source address (key for L1 cache + peer→client send)
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
 * Register a ChannelBind: updates peer_addr in snapshot, inserts channel
 * table entry, and inserts peer→alloc reverse lookup entry.
 * Called by libevent thread after successful ChannelBind.
 * client_addr / peer_addr: cast from sockaddr_storage (ioa_addr.ss)
 * channel_no: TURN channel number (0x4000–0x7FFF)
 */
void turbo_fastpath_channel_bind(uint32_t alloc_id,
                                  const void *client_addr,
                                  const void *peer_addr,
                                  uint16_t channel_no);

/*
 * Remove fastpath entries and room membership for an allocation.
 * Called from session teardown in ns_turn_server.c.
 */
void turbo_alloc_teardown(uint32_t alloc_id, const char *room_id, const char *member_id);

void *turbo_get_shared_relay_socket(void);
int   turbo_get_shared_relay_fd(void);

/*
 * Configure audit log path — must be called before turbo_init().
 * socket_path: Unix datagram socket path (e.g. "unix:///var/run/turn-audit.sock")
 *              or NULL / "" to keep audit in-memory only (no socket flush).
 */
void turbo_audit_configure(const char *socket_path);

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
