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
#include "common/turbo_ring.h"

#define TURBO_RELAY_PORT 3478

/* Global instances (initialised by turbo_init()) */
extern struct turbo_netif      g_turbo_netif;
extern struct turbo_ring       g_ctrl_ring;   /* Worker → libevent STUN queue */

/* Shared relay socket used by all allocations in single-port mode */
extern void *turbo_shared_relay_socket;       /* ioa_socket_handle */

/* Atomic flag set by SIGUSR1 handler; checked by worker loop */
extern _Atomic int             turbo_degrade_requested;

int  turbo_init(void);
void turbo_deinit(void);

/* Called by libevent thread after successful Allocate */
void turbo_l1_cache_warmup(void *ss, const void *client_addr);
void turbo_override_relay_port(void *ss, uint16_t port);

void *turbo_get_shared_relay_socket(void);
int   turbo_get_shared_relay_fd(void);

#endif /* TURBO_FEATURES */
#endif /* TURBO_H */
