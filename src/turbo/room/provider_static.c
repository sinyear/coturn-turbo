/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Static room identity provider.
 * Design doc §8.2.
 *
 * Parses TURN username of the form "room<ID>:<member_id>",
 * e.g. "room123:userA".  No secret or token required.
 */

#include "turbo_room_provider.h"
#include <stdio.h>
#include <string.h>

static int static_init(void *config) {
    (void)config;
    return 0;
}

static int static_extract(const char *username, const char *realm,
                           const char *credential,
                           const struct sockaddr *client_addr,
                           struct turbo_room_info *out) {
    (void)realm;
    (void)credential;
    (void)client_addr;

    if (!username || !out) return -1;

    char room_id[64]   = {0};
    char member_id[64] = {0};

    /* Expected format: "room<ID>:<member_id>" */
    if (sscanf(username, "room%63[^:]:%63s", room_id, member_id) != 2)
        return -1;
    if (room_id[0] == '\0' || member_id[0] == '\0')
        return -1;

    strncpy(out->room_id,   room_id,   sizeof(out->room_id)   - 1);
    strncpy(out->member_id, member_id, sizeof(out->member_id) - 1);
    out->expiry = 0;  /* static provider: no expiry */
    return 0;
}

static void static_deinit(void) {}

struct turbo_room_provider_ops provider_static_ops = {
    .name    = "static",
    .init    = static_init,
    .extract = static_extract,
    .deinit  = static_deinit,
};
