/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Three-table fast-path lookup engine implementation.
 * Design doc §4.3 (lookup flow) and §6.2 (seqlock concurrency).
 */

#include "turbo_fastpath.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/* Internal hash helpers                                               */
/* ------------------------------------------------------------------ */

/* Fast 64-bit mixing hash (FNV-1a style, fits in registers) */
static inline uint64_t mix64(uint64_t v) {
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return v;
}

/* Hash (src_ip[16] | src_port[2] | channel_no[2]) → channel_table key */
static uint64_t channel_key(const struct sockaddr_in6 *src, uint16_t channel_no) {
    uint64_t a = 0, b = 0;
    memcpy(&a, &src->sin6_addr.s6_addr[0], 8);
    memcpy(&b, &src->sin6_addr.s6_addr[8], 8);
    uint64_t v = mix64(a ^ b ^ (uint64_t)src->sin6_port ^ ((uint64_t)channel_no << 16));
    return v ? v : 1;  /* avoid key=0 (means unused) */
}

/* Hash (src_ip | src_port) → l1_cache key */
static uint64_t l1_key(const struct sockaddr_in6 *src) {
    uint64_t a = 0, b = 0;
    memcpy(&a, &src->sin6_addr.s6_addr[0], 8);
    memcpy(&b, &src->sin6_addr.s6_addr[8], 8);
    uint64_t v = mix64(a ^ b ^ (uint64_t)src->sin6_port);
    return v ? v : 1;
}

/* djb2 hash of a username C string */
static uint64_t username_hash(const char *s, size_t len) {
    uint64_t h = 5381;
    for (size_t i = 0; i < len; i++)
        h = ((h << 5) + h) + (uint8_t)s[i];
    return h ? h : 1;
}

/* ------------------------------------------------------------------ */
/* Open-addressing lookup helpers (linear probe, no deletion)          */
/* ------------------------------------------------------------------ */

#define CHANNEL_IDX(key)  ((uint32_t)((key) % TURBO_CHANNEL_TABLE_SIZE))
#define L1_IDX(key)       ((uint32_t)((key) % TURBO_L1_CACHE_SIZE))
#define USERNAME_IDX(key) ((uint32_t)((key) % TURBO_USERNAME_TABLE_SIZE))

static uint32_t channel_lookup(const struct turbo_fastpath *fp,
                                uint64_t key) {
    uint32_t idx = CHANNEL_IDX(key);
    for (int i = 0; i < 8; i++) {
        uint32_t slot = (idx + i) % TURBO_CHANNEL_TABLE_SIZE;
        if (!fp->channel_table[slot].used) break;
        if (fp->channel_table[slot].key == key)
            return fp->channel_table[slot].alloc_id;
    }
    return 0;
}

static uint32_t l1_lookup(const struct turbo_fastpath *fp, uint64_t key) {
    uint32_t idx = L1_IDX(key);
    for (int i = 0; i < 8; i++) {
        uint32_t slot = (idx + i) % TURBO_L1_CACHE_SIZE;
        if (!fp->l1_cache[slot].used) break;
        if (fp->l1_cache[slot].key == key)
            return fp->l1_cache[slot].alloc_id;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parse helpers                                                        */
/* ------------------------------------------------------------------ */

/* Check if the 2-byte prefix is a STUN message (RFC 5389 §6) */
static inline int is_stun(const uint8_t *buf) {
    /* Top 2 bits must be 0 */
    return (buf[0] & 0xC0) == 0x00;
}

/* Check if the 2-byte prefix is a ChannelData (0x4000–0x7FFF) */
static inline int is_channel_data(const uint8_t *buf) {
    uint16_t type = ((uint16_t)buf[0] << 8) | buf[1];
    return type >= 0x4000 && type <= 0x7FFF;
}

/* Extract channel number from ChannelData header */
static inline uint16_t channel_no_from_buf(const uint8_t *buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

/* Locate the USERNAME attribute in a STUN message.
 * Returns pointer to the value field, writes *username_len, or NULL. */
static const uint8_t *find_username_attr(const uint8_t *buf, size_t len,
                                          size_t *username_len) {
    if (len < 20) return NULL;  /* STUN header is 20 bytes */
    size_t off = 20;
    while (off + 4 <= len) {
        uint16_t atype = ((uint16_t)buf[off] << 8) | buf[off+1];
        uint16_t alen  = ((uint16_t)buf[off+2] << 8) | buf[off+3];
        if (atype == 0x0006) {  /* USERNAME */
            if (username_len) *username_len = alen;
            return buf + off + 4;
        }
        /* Advance past attribute value (padded to 4-byte boundary) */
        off += 4 + ((alen + 3) & ~3u);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Init / deinit                                                        */
/* ------------------------------------------------------------------ */

int turbo_fastpath_init(struct turbo_fastpath *fp) {
    memset(fp, 0, sizeof(*fp));
    fp->next_alloc_id = 1;  /* 0 is TURBO_ALLOC_ID_INVALID */
    return pthread_mutex_init(&fp->write_lock, NULL);
}

void turbo_fastpath_deinit(struct turbo_fastpath *fp) {
    pthread_mutex_destroy(&fp->write_lock);
}

/* ------------------------------------------------------------------ */
/* Lookup — hot path, no locks on read side                            */
/* ------------------------------------------------------------------ */

uint32_t turbo_fastpath_lookup(struct turbo_fastpath *fp,
                                const uint8_t *buf, size_t len,
                                const struct sockaddr_in6 *src) {
    if (len < 2) return 0;

    /* Layer 1: ChannelData — fastest path */
    if (is_channel_data(buf)) {
        uint16_t ch = channel_no_from_buf(buf);
        uint64_t key = channel_key(src, ch);
        uint32_t id  = channel_lookup(fp, key);
        if (id) {
            atomic_fetch_add_explicit(&fp->channel_hits, 1, __ATOMIC_RELAXED);
            return id;
        }
        atomic_fetch_add_explicit(&fp->misses, 1, __ATOMIC_RELAXED);
        return 0;
    }

    if (!is_stun(buf)) {
        atomic_fetch_add_explicit(&fp->misses, 1, __ATOMIC_RELAXED);
        return 0;
    }

    /* Layer 2: L1 cache — Send Indication (type 0x0016) or Data Indication */
    uint64_t l1k = l1_key(src);
    uint32_t id  = l1_lookup(fp, l1k);
    if (id) {
        atomic_fetch_add_explicit(&fp->l1_hits, 1, __ATOMIC_RELAXED);
        return id;
    }

    /* Layer 3: USERNAME attribute lookup — fallback */
    size_t ulen = 0;
    const uint8_t *uval = find_username_attr(buf, len, &ulen);
    if (uval && ulen > 0 && ulen < 128) {
        uint64_t uhash = username_hash((const char *)uval, ulen);
        uint32_t idx = USERNAME_IDX(uhash);
        for (int i = 0; i < 8; i++) {
            uint32_t slot = (idx + i) % TURBO_USERNAME_TABLE_SIZE;
            if (!fp->username_table[slot].used) break;
            if (fp->username_table[slot].key == uhash) {
                id = fp->username_table[slot].alloc_id;
                atomic_fetch_add_explicit(&fp->username_hits, 1, __ATOMIC_RELAXED);
                /* Back-fill L1 cache (libevent side would normally do this;
                 * this is an emergency repair path) */
                pthread_mutex_lock(&fp->write_lock);
                uint32_t l1_slot = L1_IDX(l1k);
                for (int j = 0; j < TURBO_L1_CACHE_SIZE; j++) {
                    uint32_t s = (l1_slot + j) % TURBO_L1_CACHE_SIZE;
                    if (!fp->l1_cache[s].used || fp->l1_cache[s].key == l1k) {
                        fp->l1_cache[s].key      = l1k;
                        fp->l1_cache[s].alloc_id = id;
                        fp->l1_cache[s].used     = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&fp->write_lock);
                return id;
            }
        }
    }

    atomic_fetch_add_explicit(&fp->misses, 1, __ATOMIC_RELAXED);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Seqlock-protected allocation snapshot read                           */
/* ------------------------------------------------------------------ */

int turbo_fastpath_read_alloc(struct turbo_fastpath *fp,
                               uint32_t alloc_id,
                               struct turbo_alloc_snapshot *snap) {
    if (alloc_id == 0 || alloc_id >= TURBO_ALLOC_TABLE_SIZE) return -1;

    struct turbo_alloc_entry *e = &fp->alloc_table[alloc_id];
    if (!e->used) return -1;

    uint64_t seq;
    do {
        seq = turbo_seqlock_read_begin(&e->seqlock);
        *snap = e->snap;
    } while (turbo_seqlock_read_retry(&e->seqlock, seq));

    /* Check expiry */
    if (snap->expiry != 0 && time(NULL) > (time_t)snap->expiry)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Write-side helpers (called from libevent thread, lock protected)    */
/* ------------------------------------------------------------------ */

void turbo_fastpath_warmup(struct turbo_fastpath *fp,
                            uint32_t alloc_id,
                            const struct sockaddr_in6 *client_addr,
                            const struct sockaddr_in6 *peer_addr,
                            const char *room_id,
                            uint64_t expiry) {
    if (alloc_id == 0 || alloc_id >= TURBO_ALLOC_TABLE_SIZE) return;

    pthread_mutex_lock(&fp->write_lock);

    /* Write allocation snapshot */
    struct turbo_alloc_entry *e = &fp->alloc_table[alloc_id];
    turbo_seqlock_write_begin(&e->seqlock);
    e->snap.alloc_id = alloc_id;
    if (client_addr) e->snap.client_addr = *client_addr;
    if (peer_addr)   e->snap.peer_addr   = *peer_addr;
    if (room_id)     strncpy(e->snap.room_id, room_id, 63);
    e->snap.expiry  = expiry;
    e->used         = 1;
    turbo_seqlock_write_end(&e->seqlock);

    /* Insert into L1 cache */
    uint64_t key  = l1_key(client_addr);
    uint32_t idx  = L1_IDX(key);
    for (int i = 0; i < TURBO_L1_CACHE_SIZE; i++) {
        uint32_t slot = (idx + i) % TURBO_L1_CACHE_SIZE;
        if (!fp->l1_cache[slot].used || fp->l1_cache[slot].key == key) {
            fp->l1_cache[slot].key      = key;
            fp->l1_cache[slot].alloc_id = alloc_id;
            fp->l1_cache[slot].used     = 1;
            break;
        }
    }

    pthread_mutex_unlock(&fp->write_lock);
}

void turbo_fastpath_add_channel(struct turbo_fastpath *fp,
                                 uint32_t alloc_id,
                                 const struct sockaddr_in6 *client_addr,
                                 uint16_t channel_no) {
    pthread_mutex_lock(&fp->write_lock);

    uint64_t key = channel_key(client_addr, channel_no);
    uint32_t idx = CHANNEL_IDX(key);
    for (int i = 0; i < TURBO_CHANNEL_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) % TURBO_CHANNEL_TABLE_SIZE;
        if (!fp->channel_table[slot].used || fp->channel_table[slot].key == key) {
            fp->channel_table[slot].key      = key;
            fp->channel_table[slot].alloc_id = alloc_id;
            fp->channel_table[slot].used     = 1;
            break;
        }
    }

    pthread_mutex_unlock(&fp->write_lock);
}

void turbo_fastpath_remove(struct turbo_fastpath *fp, uint32_t alloc_id) {
    if (alloc_id == 0 || alloc_id >= TURBO_ALLOC_TABLE_SIZE) return;

    pthread_mutex_lock(&fp->write_lock);

    /* Mark alloc entry as unused */
    struct turbo_alloc_entry *e = &fp->alloc_table[alloc_id];
    turbo_seqlock_write_begin(&e->seqlock);
    e->used = 0;
    memset(&e->snap, 0, sizeof(e->snap));
    turbo_seqlock_write_end(&e->seqlock);

    /* Invalidate L1 entries pointing to this alloc_id */
    for (int i = 0; i < TURBO_L1_CACHE_SIZE; i++) {
        if (fp->l1_cache[i].alloc_id == alloc_id)
            fp->l1_cache[i].used = 0;
    }

    /* Invalidate channel entries */
    for (int i = 0; i < TURBO_CHANNEL_TABLE_SIZE; i++) {
        if (fp->channel_table[i].alloc_id == alloc_id)
            fp->channel_table[i].used = 0;
    }

    /* Invalidate username entries */
    for (int i = 0; i < TURBO_USERNAME_TABLE_SIZE; i++) {
        if (fp->username_table[i].alloc_id == alloc_id)
            fp->username_table[i].used = 0;
    }

    /* Invalidate peer reverse entries */
    for (int i = 0; i < TURBO_PEER_REV_TABLE_SIZE; i++) {
        if (fp->peer_rev_table[i].alloc_id == alloc_id)
            fp->peer_rev_table[i].used = 0;
    }

    pthread_mutex_unlock(&fp->write_lock);
}

/* ------------------------------------------------------------------ */
/* Alloc-ID slot allocator (P6: collision-free assignment)             */
/* ------------------------------------------------------------------ */

uint32_t turbo_fastpath_alloc_id_acquire(struct turbo_fastpath *fp) {
    pthread_mutex_lock(&fp->write_lock);
    uint32_t found = 0;
    for (uint32_t i = 0; i < TURBO_ALLOC_TABLE_SIZE - 1; i++) {
        uint32_t id = fp->next_alloc_id;
        fp->next_alloc_id = (fp->next_alloc_id % (TURBO_ALLOC_TABLE_SIZE - 1)) + 1;
        if (!fp->alloc_table[id].used) {
            found = id;
            break;
        }
    }
    pthread_mutex_unlock(&fp->write_lock);
    return found;  /* 0 = table full */
}

void turbo_fastpath_alloc_id_release(struct turbo_fastpath *fp, uint32_t alloc_id) {
    /* turbo_fastpath_remove already clears the entry; this is a no-op stub
     * kept so callers don't need to know the internals. */
    (void)fp;
    (void)alloc_id;
}

/* ------------------------------------------------------------------ */
/* Peer reverse-lookup table                                           */
/* ------------------------------------------------------------------ */

/* Use same mix as l1_key — peer key is also (ip, port) */
static uint64_t peer_rev_key(const struct sockaddr_in6 *src) {
    return l1_key(src);
}

#define PEER_REV_IDX(key)  ((uint32_t)((key) % TURBO_PEER_REV_TABLE_SIZE))

void turbo_fastpath_update_peer(struct turbo_fastpath *fp,
                                 uint32_t alloc_id,
                                 const struct sockaddr_in6 *peer_addr) {
    if (!alloc_id || alloc_id >= TURBO_ALLOC_TABLE_SIZE || !peer_addr) return;

    struct turbo_alloc_entry *e = &fp->alloc_table[alloc_id];
    pthread_mutex_lock(&fp->write_lock);
    turbo_seqlock_write_begin(&e->seqlock);
    e->snap.peer_addr = *peer_addr;
    turbo_seqlock_write_end(&e->seqlock);
    pthread_mutex_unlock(&fp->write_lock);
}

void turbo_fastpath_add_peer_rev(struct turbo_fastpath *fp,
                                  uint32_t alloc_id,
                                  const struct sockaddr_in6 *peer_addr,
                                  uint16_t channel_no) {
    if (!alloc_id || !peer_addr) return;
    pthread_mutex_lock(&fp->write_lock);

    uint64_t key = peer_rev_key(peer_addr);
    uint32_t idx = PEER_REV_IDX(key);
    for (int i = 0; i < TURBO_PEER_REV_TABLE_SIZE; i++) {
        uint32_t slot = (idx + i) % TURBO_PEER_REV_TABLE_SIZE;
        if (!fp->peer_rev_table[slot].used || fp->peer_rev_table[slot].key == key) {
            fp->peer_rev_table[slot].key        = key;
            fp->peer_rev_table[slot].alloc_id   = alloc_id;
            fp->peer_rev_table[slot].channel_no = channel_no;
            fp->peer_rev_table[slot].used       = 1;
            break;
        }
    }

    pthread_mutex_unlock(&fp->write_lock);
}

uint32_t turbo_fastpath_lookup_peer_rev(struct turbo_fastpath *fp,
                                         const struct sockaddr_in6 *src,
                                         uint16_t *channel_no_out) {
    uint64_t key = peer_rev_key(src);
    uint32_t idx = PEER_REV_IDX(key);
    for (int i = 0; i < 8; i++) {
        uint32_t slot = (idx + i) % TURBO_PEER_REV_TABLE_SIZE;
        if (!fp->peer_rev_table[slot].used) break;
        if (fp->peer_rev_table[slot].key == key) {
            if (channel_no_out)
                *channel_no_out = fp->peer_rev_table[slot].channel_no;
            return fp->peer_rev_table[slot].alloc_id;
        }
    }
    return 0;
}
