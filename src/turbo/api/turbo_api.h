/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Admin HTTP API for coturn-turbo.
 * Design doc §11.
 *
 * Endpoints:
 *   GET  /admin/status        — JSON operational summary (§11.2)
 *   GET  /admin/metrics       — Prometheus text metrics (§11.1)
 *   POST /admin/turbo-disable — degrade backend to epoll at runtime
 *   POST /admin/turbo-enable  — reset degrade-request flag
 *   POST /admin/drain         — stop accepting new allocations
 *   GET  /admin/drain/status  — drain progress report
 */

#ifndef TURBO_API_H
#define TURBO_API_H

#include <stdint.h>

/*
 * Start the admin HTTP server in a background thread.
 * port == 0 → server not started (no-op, returns 0).
 */
int  turbo_api_start(uint16_t port);

/* Signal the API server to stop and wait for the background thread. */
void turbo_api_stop(void);

#endif /* TURBO_API_H */
