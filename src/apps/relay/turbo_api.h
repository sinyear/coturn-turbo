/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * https://opensource.org/license/bsd-3-clause
 *
 * Copyright (C) 2011, 2012, 2013 Citrix Systems
 *
 * All rights reserved.
 */

#ifndef __TURBO_API__
#define __TURBO_API__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the turbo room management HTTP API server
 * @param port Port to listen on
 * @return 0 on success, -1 on failure
 */
int turbo_api_start(uint16_t port);

/**
 * Stop the turbo room management HTTP API server
 */
void turbo_api_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __TURBO_API__ */
