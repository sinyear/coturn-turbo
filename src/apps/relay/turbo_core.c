#include "turbo_core.h"
#include "mainrelay.h"
#include "ns_turn_utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <netinet/in.h>
#include "turbo_port.h"

#define TURBO_MAX_SESSIONS 65536

/* Poll loop: receive packets, process through turbo fast path */
static void* turbo_core_poll_thread(void *arg) {
    struct turbo_core *core = (struct turbo_core *)arg;
    struct turbo_packet *pkts[TURBO_RX_BURST] = {0};
    int reclaim_counter = 0;

    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo_core: poll thread started\n");

    while (atomic_load(&core->running)) {
        /* Receive burst of packets */
        uint16_t nb_rx = core->netif->ops->rx_burst(core->netif, pkts, TURBO_RX_BURST);

        if (nb_rx == 0) {
            /* No packets available; brief sleep to avoid busy-waiting */
            usleep(TURBO_POLL_INTERVAL_US);
            continue;
        }

        core->rx_packets += nb_rx;

        /* Process each packet through turbo fast path */
        for (uint16_t i = 0; i < nb_rx; i++) {
            if (!pkts[i]) {
                core->rx_dropped++;
                continue;
            }

            core->rx_bytes += pkts[i]->len;

            int ret = turbo_core_process_packet(core, pkts[i]);
            if (ret != 0) {
                core->rx_dropped++;
            }

            /* Free the original packet */
            core->netif->ops->free_pkt(core->netif, pkts[i]);
            pkts[i] = NULL;
        }

        /* Periodically reclaim RCU deferred deletions (every 1024 packets) */
        if (++reclaim_counter >= 16) {
            turbo_port_map_reclaim(&core->port_map);
            turbo_room_mgr_reclaim(core->room_mgr);
            reclaim_counter = 0;
        }
    }

    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo_core: poll thread exiting\n");
    return NULL;
}

/* Initialize turbo core engine */
int turbo_core_init(struct turbo_core *core,
                    struct turbo_netif *netif,
                    struct turbo_room_mgr *room_mgr) {
    if (!core || !netif || !room_mgr) {
        return -1;
    }

    memset(core, 0, sizeof(*core));
    core->netif = netif;
    core->room_mgr = room_mgr;
    atomic_store(&core->running, 0);

    /* Initialize port map for single-port multiplexing */
    int ret = turbo_port_map_init(&core->port_map, TURBO_MAX_SESSIONS);
    if (ret != 0) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo_core: failed to init port map (%d)\n", ret);
        return ret;
    }

    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo_core: engine initialized\n");
    return 0;
}

/* Start poll loop */
int turbo_core_start(struct turbo_core *core) {
    if (!core) {
        return -1;
    }

    if (atomic_load(&core->running)) {
        TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "turbo_core: already running\n");
        return 0;
    }

    atomic_store(&core->running, 1);

    if (pthread_create(&core->poll_thread, NULL, turbo_core_poll_thread, core) != 0) {
        atomic_store(&core->running, 0);
        TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "turbo_core: failed to create poll thread\n");
        return -1;
    }

    /* Detach so it doesn't need explicit join */
    pthread_detach(core->poll_thread);

    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo_core: poll loop started\n");
    return 0;
}

/* Stop poll loop */
void turbo_core_stop(struct turbo_core *core) {
    if (!core) {
        return;
    }

    if (atomic_load(&core->running)) {
        atomic_store(&core->running, 0);
        TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turbo_core: stopping poll loop\n");
    }
}

/* Cleanup */
void turbo_core_cleanup(struct turbo_core *core) {
    if (!core) {
        return;
    }

    turbo_core_stop(core);

    /* Brief wait for thread to notice the flag */
    usleep(50000); /* 50ms */

    turbo_port_map_cleanup(&core->port_map);
    memset(core, 0, sizeof(*core));
}

/* Process a single packet through turbo fast path */
int turbo_core_process_packet(struct turbo_core *core, struct turbo_packet *pkt) {
    if (!core || !pkt || !pkt->data) {
        return -1;
    }

    /* Extract five-tuple from packet */
    struct turbo_five_tuple tuple = {0};
    if (turbo_extract_five_tuple(pkt->data, pkt->len, &tuple) != 0) {
        /* Not a valid UDP/IP packet; let it fall back to STUN/TURN path */
        return -1;
    }

    /* Look up session mapping in port map */
    uint32_t room_id = 0, member_id = 0;
    if (turbo_port_map_lookup(&core->port_map, &tuple, &room_id, &member_id) == 0) {
        /* Fast path hit: broadcast to room members */
        core->lookup_hits++;

        int sent = turbo_room_broadcast(core->room_mgr, room_id, member_id, pkt);
        core->tx_packets += sent;

        /* Estimate bytes: clone packets have same payload size */
        /* We don't have exact size after rewrite, but approximate */
        core->tx_bytes += sent * pkt->len;

        return 0;
    }

    /* Fast path miss: packet will fall back to native STUN/TURN processing */
    core->lookup_misses++;
    return -1;
}

/* Register session mapping */
int turbo_core_register_session(struct turbo_core *core,
                                uint32_t room_id, uint32_t member_id,
                                uint32_t src_addr, uint16_t src_port) {
    if (!core) {
        return -1;
    }

    /* Build five-tuple for session registration.
     * The src_addr is stored as 4-byte IPv4 in the current API.
     * We use the new extended format with af field. */
    struct turbo_five_tuple tuple = {0};
    tuple.af = TURBO_AF_INET;
    memcpy(tuple.src_addr, &src_addr, 4);
    tuple.src_port = src_port;
    /* dst_addr/dst_port are set to 0 (wildcard match) */
    tuple.proto = IPPROTO_UDP;

    return turbo_port_map_insert(&core->port_map, &tuple, room_id, member_id);
}

/* Register session mapping (IPv6-capable) */
int turbo_core_register_session_af(struct turbo_core *core,
                                   uint32_t room_id, uint32_t member_id,
                                   const uint8_t *src_addr, size_t addr_len,
                                   uint16_t src_port, uint8_t af) {
    if (!core || !src_addr) {
        return -1;
    }

    size_t expected_len = (af == TURBO_AF_INET6) ? 16 : 4;
    if (addr_len < expected_len) {
        return -1;
    }

    struct turbo_five_tuple tuple = {0};
    tuple.af = af;
    memcpy(tuple.src_addr, src_addr, expected_len);
    tuple.src_port = src_port;
    tuple.proto = IPPROTO_UDP;

    return turbo_port_map_insert(&core->port_map, &tuple, room_id, member_id);
}

/* Unregister session mapping */
void turbo_core_unregister_session(struct turbo_core *core,
                                   uint32_t src_addr, uint16_t src_port) {
    if (!core) {
        return;
    }

    struct turbo_five_tuple tuple = {0};
    tuple.af = TURBO_AF_INET;
    memcpy(tuple.src_addr, &src_addr, 4);
    tuple.src_port = src_port;
    tuple.proto = IPPROTO_UDP;

    turbo_port_map_remove(&core->port_map, &tuple);
}

/* Unregister session mapping (IPv6-capable) */
void turbo_core_unregister_session_af(struct turbo_core *core,
                                      const uint8_t *src_addr, size_t addr_len,
                                      uint16_t src_port, uint8_t af) {
    if (!core || !src_addr) {
        return;
    }

    size_t expected_len = (af == TURBO_AF_INET6) ? 16 : 4;
    if (addr_len < expected_len) {
        return;
    }

    struct turbo_five_tuple tuple = {0};
    tuple.af = af;
    memcpy(tuple.src_addr, src_addr, expected_len);
    tuple.src_port = src_port;
    tuple.proto = IPPROTO_UDP;

    turbo_port_map_remove(&core->port_map, &tuple);
}

/* Get stats */
void turbo_core_get_stats(struct turbo_core *core, turbo_core_stats_t *stats) {
    if (!core || !stats) {
        return;
    }

    stats->rx_packets = core->rx_packets;
    stats->tx_packets = core->tx_packets;
    stats->rx_bytes = core->rx_bytes;
    stats->tx_bytes = core->tx_bytes;
    stats->rx_dropped = core->rx_dropped;
    stats->tx_dropped = core->tx_dropped;
    stats->lookup_hits = core->lookup_hits;
    stats->lookup_misses = core->lookup_misses;
}

/* Reset stats */
void turbo_core_reset_stats(struct turbo_core *core) {
    if (!core) {
        return;
    }

    core->rx_packets = 0;
    core->tx_packets = 0;
    core->rx_bytes = 0;
    core->tx_bytes = 0;
    core->rx_dropped = 0;
    core->tx_dropped = 0;
    core->lookup_hits = 0;
    core->lookup_misses = 0;
}

