/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Room Identity Provider interface.
 * Design doc §8.1.
 *
 * Each inbound Allocate request can be inspected by a provider which
 * extracts a (room_id, member_id) pair from the TURN username/credential.
 *
 * Return codes from extract():
 *   0   — success; room_id / member_id / expiry are populated
 *  -1   — username does not match this provider's format; treat as plain TURN
 *  -2   — authentication failed (e.g. bad HMAC, expired token); reject with 401
 */

#ifndef TURBO_ROOM_PROVIDER_H
#define TURBO_ROOM_PROVIDER_H

#include <stdint.h>
#include <netinet/in.h>

/* Information extracted from the TURN username token */
struct turbo_room_info {
    char     room_id[64];    /* NUL-terminated room identifier */
    char     member_id[64];  /* NUL-terminated member identifier */
    uint64_t expiry;         /* UNIX timestamp; 0 = no expiry */
};

/* Provider interface */
struct turbo_room_provider_ops {
    const char *name;

    /* One-time init (called at server startup).
     * config: opaque pointer to provider-specific settings. */
    int  (*init)(void *config);

    /* Extract room info from TURN credentials.
     * Returns 0 (hit), -1 (miss), or -2 (auth failure). */
    int  (*extract)(const char *username, const char *realm,
                    const char *credential, const struct sockaddr *client_addr,
                    struct turbo_room_info *out);

    /* Cleanup (called at server shutdown). */
    void (*deinit)(void);
};

/* Built-in providers */
extern struct turbo_room_provider_ops provider_static_ops;
extern struct turbo_room_provider_ops provider_token_hmac_ops;

/* Lookup provider by name string ("static", "token_hmac") */
struct turbo_room_provider_ops *turbo_provider_get(const char *name);

/* token_hmac provider configuration (call before init) */
void turbo_token_hmac_set_secret(const char *secret, size_t len);
void turbo_token_hmac_set_max_expiry(uint64_t seconds);

#endif /* TURBO_ROOM_PROVIDER_H */
