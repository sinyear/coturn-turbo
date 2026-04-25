#ifndef TURBO_SWITCH_H
#define TURBO_SWITCH_H

#include "turbo_netif.h"
#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

/**
 * IP/UDP header rewrite for broadcast.
 * Rewrites destination IP and port, recalculates checksums.
 * Supports both IPv4 and IPv6.
 */

/* Compute IPv4 header checksum */
uint16_t turbo_ipv4_checksum(void *iphdr, size_t len);

/**
 * Compute UDP checksum for IPv4 packet.
 * @param pkt Packet data (starts at IP header)
 * @param pkt_len Packet length
 * @param src_addr Source IPv4 address (network byte order)
 * @param dst_addr Dest IPv4 address (network byte order)
 * @return UDP checksum
 */
uint16_t turbo_udp_checksum(const uint8_t *pkt, size_t pkt_len,
                            uint32_t src_addr, uint32_t dst_addr);

/**
 * Compute UDP checksum for IPv6 packet (pseudo-header).
 * @param pkt Packet data (starts at IPv6 header)
 * @param pkt_len Packet length
 * @param src_addr Source IPv6 address
 * @param dst_addr Dest IPv6 address
 * @return UDP checksum
 */
uint16_t turbo_udp6_checksum(const uint8_t *pkt, size_t pkt_len,
                             const uint8_t *src_addr, const uint8_t *dst_addr);

/**
 * Rewrite IP/UDP destination address and port in a cloned packet.
 * Supports both IPv4 and IPv6.
 *
 * Packet layout (raw L3):
 *   IPv4: [IP header (20+)] [UDP header (8)] [payload]
 *   IPv6: [IPv6 header (40)] [UDP header (8)] [payload]
 *
 * @param pkt Cloned packet (data points to IP header)
 * @param dst_addr New destination IP (network byte order, 4 or 16 bytes)
 * @param dst_port New destination port (network byte order)
 * @param af Address family (4 for IPv4, 6 for IPv6)
 * @return 0 on success, -1 on failure
 */
int turbo_switch_rewrite_dst(struct turbo_packet *pkt,
                             const uint8_t *dst_addr, uint16_t dst_port,
                             uint8_t af);

/**
 * Rewrite IP/UDP source address and port.
 *
 * @param pkt Packet (data points to IP header)
 * @param src_addr New source IP (network byte order, 4 or 16 bytes)
 * @param src_port New source port (network byte order)
 * @param af Address family (4 for IPv4, 6 for IPv6)
 * @return 0 on success, -1 on failure
 */
int turbo_switch_rewrite_src(struct turbo_packet *pkt,
                             const uint8_t *src_addr, uint16_t src_port,
                             uint8_t af);

/**
 * Broadcast packet to multiple room members with per-member header rewrite.
 * Collects all cloned packets into a batch array for burst TX.
 *
 * @param netif Network interface
 * @param pkt Original packet to broadcast
 * @param dst_addrs Array of destination addresses (each TURBO_MAX_ADDR_LEN bytes)
 * @param dst_ports Array of destination ports (network byte order)
 * @param num_dsts Number of destinations
 * @param af Address family (4 or 6)
 * @param tx_array Output array of cloned packets for burst TX
 * @param tx_array_size Size of tx_array (max entries)
 * @return Number of packets prepared for TX
 */
int turbo_switch_broadcast(struct turbo_netif *netif,
                           struct turbo_packet *pkt,
                           const uint8_t *dst_addrs,
                           const uint16_t *dst_ports,
                           int num_dsts,
                           uint8_t af,
                           struct turbo_packet **tx_array,
                           int tx_array_size);

/**
 * Forward a single packet: clone, rewrite dest, enqueue for TX.
 *
 * @param netif Network interface
 * @param pkt Original packet
 * @param dst_addr Destination IP (4 or 16 bytes)
 * @param dst_port Destination port (network byte order)
 * @param af Address family
 * @return 0 on success, -1 on failure
 */
int turbo_switch_forward_one(struct turbo_netif *netif,
                             struct turbo_packet *pkt,
                             const uint8_t *dst_addr, uint16_t dst_port,
                             uint8_t af);

#endif /* TURBO_SWITCH_H */
