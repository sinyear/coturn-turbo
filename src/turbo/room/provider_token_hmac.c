/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * HMAC-SHA256 token room identity provider.
 * Design doc §8.3.
 *
 * Token format (JWT-like, dot-separated, base64url encoded):
 *   <header_b64>.<payload_b64>.<signature_b64>
 *
 * Header (ignored, any valid base64url accepted).
 * Payload (JSON subset): {"room_id":"...","member_id":"...","exp":NNN}
 * Signature: HMAC-SHA256(header + "." + payload, shared_secret)
 *
 * Configuration (call turbo_token_hmac_configure() before init):
 *   TURBO_ROOM_SECRET env var  OR  turbo_token_hmac_set_secret()
 */

#include "turbo_room_provider.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>

/* ------------------------------------------------------------------ */
/* Configuration state                                                  */
/* ------------------------------------------------------------------ */

static char  g_secret[256]     = {0};
static size_t g_secret_len     = 0;
static uint64_t g_max_expiry   = 0;  /* 0 = unlimited */

void turbo_token_hmac_set_secret(const char *secret, size_t len);
void turbo_token_hmac_set_max_expiry(uint64_t seconds);

void turbo_token_hmac_set_secret(const char *secret, size_t len) {
    if (!secret || len == 0 || len >= sizeof(g_secret)) return;
    memcpy(g_secret, secret, len);
    g_secret_len = len;
}

void turbo_token_hmac_set_max_expiry(uint64_t seconds) {
    g_max_expiry = seconds;
}

/* ------------------------------------------------------------------ */
/* Base64url decode                                                     */
/* ------------------------------------------------------------------ */

static const char b64url_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64url_decode(const char *in, size_t in_len,
                          uint8_t *out, size_t *out_len) {
    static int8_t dec[256];
    static int initialised = 0;
    if (!initialised) {
        memset(dec, -1, sizeof(dec));
        for (int i = 0; i < 64; i++)
            dec[(uint8_t)b64url_table[i]] = (int8_t)i;
        initialised = 1;
    }

    size_t n = 0;
    uint32_t bits = 0;
    int bit_count = 0;

    for (size_t i = 0; i < in_len; i++) {
        int8_t v = dec[(uint8_t)in[i]];
        if (v < 0) continue;  /* skip padding / unknown chars */
        bits = (bits << 6) | (uint32_t)v;
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            if (n >= *out_len) return -1;
            out[n++] = (uint8_t)((bits >> bit_count) & 0xFF);
        }
    }
    *out_len = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON field extraction (no allocations)                       */
/* ------------------------------------------------------------------ */

/* Extract string value for a given key from a flat JSON object.
 * Writes at most out_size-1 chars plus NUL.  Returns 0 on success. */
static int json_extract_str(const char *json, size_t json_len,
                             const char *key, char *out, size_t out_size) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = json;
    const char *end = json + json_len;

    while (p < end) {
        const char *found = (const char *)memmem(p, (size_t)(end - p),
                                                   search, strlen(search));
        if (!found) return -1;
        p = found + strlen(search);
        while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) p++;
        if (p >= end || *p != '"') { p = found + 1; continue; }
        p++;  /* skip opening quote */
        size_t n = 0;
        while (p < end && *p != '"' && n + 1 < out_size)
            out[n++] = *p++;
        out[n] = '\0';
        return (*p == '"') ? 0 : -1;
    }
    return -1;
}

/* Extract integer value for a given key. */
static int json_extract_uint64(const char *json, size_t json_len,
                                const char *key, uint64_t *out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = json;
    const char *end = json + json_len;

    while (p < end) {
        const char *found = (const char *)memmem(p, (size_t)(end - p),
                                                   search, strlen(search));
        if (!found) return -1;
        p = found + strlen(search);
        while (p < end && (*p == ' ' || *p == ':' || *p == '\t')) p++;
        if (p >= end || (*p < '0' || *p > '9')) { p = found + 1; continue; }
        *out = (uint64_t)strtoull(p, NULL, 10);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Provider implementation                                              */
/* ------------------------------------------------------------------ */

static int hmac_init(void *config) {
    (void)config;
    /* Load secret from environment if not already set */
    if (g_secret_len == 0) {
        const char *env = getenv("TURBO_ROOM_SECRET");
        if (env) {
            size_t len = strlen(env);
            if (len > 0 && len < sizeof(g_secret)) {
                memcpy(g_secret, env, len);
                g_secret_len = len;
            }
        }
    }
    return (g_secret_len > 0) ? 0 : -1;
}

static int hmac_extract(const char *username, const char *realm,
                         const char *credential,
                         const struct sockaddr *client_addr,
                         struct turbo_room_info *out) {
    (void)realm;
    (void)credential;
    (void)client_addr;

    if (!username || !out || g_secret_len == 0) return -1;

    /* Split token into header.payload.signature */
    const char *dot1 = strchr(username, '.');
    if (!dot1) return -1;
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return -1;

    size_t header_len  = (size_t)(dot1 - username);
    size_t payload_len = (size_t)(dot2 - dot1 - 1);
    size_t sig_len     = strlen(dot2 + 1);

    if (header_len == 0 || payload_len == 0 || sig_len == 0)
        return -1;

    /* Compute expected HMAC over "header.payload" */
    size_t signed_len = header_len + 1 + payload_len;  /* includes the dot */
    unsigned char expected_sig[32];
    unsigned int  expected_sig_len = sizeof(expected_sig);

    if (!HMAC(EVP_sha256(),
              g_secret, (int)g_secret_len,
              (const unsigned char *)username, (unsigned int)signed_len,
              expected_sig, &expected_sig_len)) {
        return -1;
    }

    /* Decode the token's signature from base64url */
    uint8_t token_sig[64];
    size_t  token_sig_len = sizeof(token_sig);
    if (b64url_decode(dot2 + 1, sig_len, token_sig, &token_sig_len) != 0)
        return -2;
    if (token_sig_len != expected_sig_len) return -2;

    /* Constant-time comparison to prevent timing attacks */
    if (CRYPTO_memcmp(expected_sig, token_sig, expected_sig_len) != 0)
        return -2;

    /* Decode and parse payload JSON */
    uint8_t payload_json[512];
    size_t  payload_json_len = sizeof(payload_json) - 1;
    if (b64url_decode(dot1 + 1, payload_len,
                       payload_json, &payload_json_len) != 0)
        return -2;
    payload_json[payload_json_len] = '\0';

    /* Extract fields */
    if (json_extract_str((char *)payload_json, payload_json_len,
                          "room_id", out->room_id, sizeof(out->room_id)) != 0)
        return -2;

    if (json_extract_str((char *)payload_json, payload_json_len,
                          "member_id", out->member_id, sizeof(out->member_id)) != 0)
        return -2;

    uint64_t exp = 0;
    if (json_extract_uint64((char *)payload_json, payload_json_len,
                             "exp", &exp) == 0) {
        out->expiry = exp;
    } else {
        out->expiry = 0;
    }

    /* Check expiry */
    if (out->expiry != 0 && (uint64_t)time(NULL) > out->expiry)
        return -2;

    /* Check max_expiry constraint */
    if (g_max_expiry > 0 && out->expiry != 0) {
        uint64_t now = (uint64_t)time(NULL);
        if (out->expiry > now + g_max_expiry)
            return -2;
    }

    if (out->room_id[0] == '\0' || out->member_id[0] == '\0')
        return -2;

    return 0;
}

static void hmac_deinit(void) {
    /* Zero out secret material */
    if (g_secret_len > 0) {
        OPENSSL_cleanse(g_secret, g_secret_len);
        g_secret_len = 0;
    }
}

struct turbo_room_provider_ops provider_token_hmac_ops = {
    .name    = "token_hmac",
    .init    = hmac_init,
    .extract = hmac_extract,
    .deinit  = hmac_deinit,
};

/* ------------------------------------------------------------------ */
/* Registry                                                             */
/* ------------------------------------------------------------------ */

struct turbo_room_provider_ops *turbo_provider_get(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "static")     == 0) return &provider_static_ops;
    if (strcmp(name, "token_hmac") == 0) return &provider_token_hmac_ops;
    return NULL;
}
