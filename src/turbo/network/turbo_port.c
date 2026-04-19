#include "turbo_port.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>

/* IP protocol numbers */
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif

/* Minimum packet sizes */
#define MIN_IPV4_HDR_LEN sizeof(struct iphdr)
#define MIN_IPV6_HDR_LEN sizeof(struct ip6_hdr)
#define MIN_UDP_HDR_LEN  sizeof(struct udphdr)
#define MIN_IPV4_PKT_LEN (MIN_IPV4_HDR_LEN + MIN_UDP_HDR_LEN)
#define MIN_IPV6_PKT_LEN (MIN_IPV6_HDR_LEN + MIN_UDP_HDR_LEN)

/* Default hash bucket count */
#define PORT_BUCKETS_DEFAULT 4096

/* Zero-initialize address buffer */
static inline void zero_addr(uint8_t *addr) {
    memset(addr, 0, TURBO_MAX_ADDR_LEN);
}

/* Five-tuple extraction from raw IP packet (IPv4 or IPv6) */
int turbo_extract_five_tuple(const uint8_t *data, size_t len, struct turbo_five_tuple *tuple) {
    if (!data || !tuple || len < MIN_IPV4_HDR_LEN) {
        return -1;
    }

    /* Check IP version from first byte */
    uint8_t ip_version = (data[0] >> 4) & 0x0F;

    if (ip_version == 4) {
        /* IPv4 */
        if (len < MIN_IPV4_PKT_LEN) {
            return -1;
        }

        const struct iphdr *iph = (const struct iphdr *)data;
        uint8_t ip_hdr_len = (iph->ihl << 2);

        if (len < ip_hdr_len + sizeof(struct udphdr)) {
            return -1;
        }

        if (iph->protocol != IPPROTO_UDP) {
            return -1;
        }

        const struct udphdr *udph = (const struct udphdr *)(data + ip_hdr_len);

        zero_addr(tuple->src_addr);
        zero_addr(tuple->dst_addr);
        memcpy(tuple->src_addr, &iph->saddr, 4);
        memcpy(tuple->dst_addr, &iph->daddr, 4);
        tuple->src_port = udph->source;
        tuple->dst_port = udph->dest;
        tuple->proto = iph->protocol;
        tuple->af = TURBO_AF_INET;

        return 0;

    } else if (ip_version == 6) {
        /* IPv6 */
        if (len < MIN_IPV6_PKT_LEN) {
            return -1;
        }

        const struct ip6_hdr *iph6 = (const struct ip6_hdr *)data;

        /* Check next header is UDP */
        if (iph6->ip6_nxt != IPPROTO_UDP) {
            return -1;
        }

        const struct udphdr *udph = (const struct udphdr *)(data + sizeof(struct ip6_hdr));
        if (len < sizeof(struct ip6_hdr) + sizeof(struct udphdr)) {
            return -1;
        }

        memcpy(tuple->src_addr, &iph6->ip6_src, 16);
        memcpy(tuple->dst_addr, &iph6->ip6_dst, 16);
        tuple->src_port = udph->source;
        tuple->dst_port = udph->dest;
        tuple->proto = iph6->ip6_nxt;
        tuple->af = TURBO_AF_INET6;

        return 0;
    }

    /* Unsupported IP version */
    return -1;
}

/* Session entry allocation */
static struct turbo_session* session_alloc(void) {
    return (struct turbo_session *)calloc(1, sizeof(struct turbo_session));
}

static void session_free(struct turbo_session *sess) {
    free(sess);
}

/* Initialize port map */
int turbo_port_map_init(struct turbo_port_map *pmap, uint32_t max_entries) {
    if (!pmap || max_entries == 0) {
        return -EINVAL;
    }

    memset(pmap, 0, sizeof(*pmap));
    pmap->num_buckets = PORT_BUCKETS_DEFAULT;
    pmap->max_entries = max_entries;
    pmap->hash_seed = 0x5a5a5a5a;
    pmap->buckets = (struct turbo_session **)calloc(pmap->num_buckets, sizeof(struct turbo_session *));
    if (!pmap->buckets) {
        return -ENOMEM;
    }

    if (pthread_mutex_init(&pmap->lock, NULL) != 0) {
        free(pmap->buckets);
        pmap->buckets = NULL;
        return -EIO;
    }

    turbo_rcu_init(&pmap->rcu, 2048);

    return 0;
}

/* Cleanup port map */
void turbo_port_map_cleanup(struct turbo_port_map *pmap) {
    if (!pmap || !pmap->buckets) {
        return;
    }

    pthread_mutex_lock(&pmap->lock);
    for (uint32_t i = 0; i < pmap->num_buckets; i++) {
        struct turbo_session *sess = pmap->buckets[i];
        while (sess) {
            struct turbo_session *next = sess->next;
            session_free(sess);
            sess = next;
        }
    }
    free(pmap->buckets);
    pmap->buckets = NULL;
    pthread_mutex_unlock(&pmap->lock);
    pthread_mutex_destroy(&pmap->lock);
    turbo_rcu_cleanup(&pmap->rcu);
}

/* Insert session */
int turbo_port_map_insert(struct turbo_port_map *pmap,
                          const struct turbo_five_tuple *key,
                          uint32_t room_id, uint32_t member_id) {
    if (!pmap || !key || !pmap->buckets) {
        return -EINVAL;
    }

    pthread_mutex_lock(&pmap->lock);

    if (pmap->count >= pmap->max_entries) {
        pthread_mutex_unlock(&pmap->lock);
        return -ENOSPC;
    }

    uint32_t hash = turbo_five_tuple_hash(key, pmap->hash_seed);
    uint32_t bucket = hash % pmap->num_buckets;

    /* Check for duplicate */
    struct turbo_session *sess = pmap->buckets[bucket];
    while (sess) {
        if (!sess->marked_for_delete &&
            memcmp(&sess->key, key, sizeof(struct turbo_five_tuple)) == 0) {
            /* Update existing entry */
            sess->room_id = room_id;
            sess->member_id = member_id;
            pthread_mutex_unlock(&pmap->lock);
            return 0;
        }
        sess = sess->next;
    }

    /* Allocate new session */
    sess = session_alloc();
    if (!sess) {
        pthread_mutex_unlock(&pmap->lock);
        return -ENOMEM;
    }

    memcpy(&sess->key, key, sizeof(struct turbo_five_tuple));
    sess->room_id = room_id;
    sess->member_id = member_id;
    sess->hash = hash;

    /* Insert at head of bucket */
    sess->next = pmap->buckets[bucket];
    pmap->buckets[bucket] = sess;
    pmap->count++;

    pthread_mutex_unlock(&pmap->lock);
    return 0;
}

/* Lookup session (lock-free read) */
int turbo_port_map_lookup(struct turbo_port_map *pmap,
                          const struct turbo_five_tuple *key,
                          uint32_t *room_id, uint32_t *member_id) {
    if (!pmap || !key || !pmap->buckets) {
        return -1;
    }

    uint32_t hash = turbo_five_tuple_hash(key, pmap->hash_seed);
    uint32_t bucket = hash % pmap->num_buckets;

    struct turbo_session *sess = pmap->buckets[bucket];
    while (sess) {
        if (!sess->marked_for_delete && sess->hash == hash &&
            memcmp(&sess->key, key, sizeof(struct turbo_five_tuple)) == 0) {
            if (room_id) *room_id = sess->room_id;
            if (member_id) *member_id = sess->member_id;
            return 0;
        }
        sess = sess->next;
    }

    return -1;
}

/* Remove session (RCU-style mark+unlink) */
void turbo_port_map_remove(struct turbo_port_map *pmap,
                           const struct turbo_five_tuple *key) {
    if (!pmap || !key || !pmap->buckets) {
        return;
    }

    pthread_mutex_lock(&pmap->lock);

    uint32_t hash = turbo_five_tuple_hash(key, pmap->hash_seed);
    uint32_t bucket = hash % pmap->num_buckets;

    struct turbo_session *sess = pmap->buckets[bucket];
    struct turbo_session *prev = NULL;

    while (sess) {
        if (!sess->marked_for_delete && sess->hash == hash &&
            memcmp(&sess->key, key, sizeof(struct turbo_five_tuple)) == 0) {
            /* Mark for deferred deletion */
            sess->marked_for_delete = 1;

            /* Unlink from bucket */
            if (prev) {
                prev->next = sess->next;
            } else {
                pmap->buckets[bucket] = sess->next;
            }
            pmap->count--;

            /* Defer free via RCU (safe for concurrent readers) */
            turbo_rcu_defer_free(&pmap->rcu, sess);
            turbo_rcu_maybe_reclaim(&pmap->rcu, 0);
            break;
        }
        prev = sess;
        sess = sess->next;
    }

    pthread_mutex_unlock(&pmap->lock);
}

/* Reclaim deferred deletions via RCU */
void turbo_port_map_reclaim(struct turbo_port_map *pmap) {
    if (!pmap) {
        return;
    }
    turbo_rcu_maybe_reclaim(&pmap->rcu, 0);
}
