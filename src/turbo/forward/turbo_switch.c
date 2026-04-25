#include "turbo_switch.h"
#include "../netif/turbo_port.h"
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <string.h>
#include <arpa/inet.h>

/* IP protocol number */
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif

/* Address family constants */
#define AF_INET4  4
/* AF_INET6 is already defined in system headers via <netinet/in.h> */

/* IPv6 pseudo-header for UDP checksum (RFC 2460) */
struct ipv6_pseudo_hdr {
    uint8_t  src_addr[16];
    uint8_t  dst_addr[16];
    uint32_t upper_len; /* Big endian */
    uint8_t  zero[3];
    uint8_t  next_hdr;
};

/*
 * Internet checksum (RFC 1071)
 * Generic implementation that works on any buffer.
 */
static uint16_t inet_checksum(const uint16_t *buf, size_t len, uint32_t acc) {
    uint32_t sum = acc;
    size_t n = len >> 1;

    for (size_t i = 0; i < n; i++) {
        sum += buf[i];
    }

    /* Handle odd byte */
    if (len & 1) {
        sum += *(const uint8_t *)(buf + n);
    }

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/* Compute IPv4 header checksum */
uint16_t turbo_ipv4_checksum(void *iphdr, size_t len) {
    struct iphdr *ip = (struct iphdr *)iphdr;
    ip->check = 0;
    return inet_checksum((const uint16_t *)iphdr, len, 0);
}

/* Compute UDP checksum for IPv4 */
uint16_t turbo_udp_checksum(const uint8_t *pkt, size_t pkt_len,
                            uint32_t src_addr, uint32_t dst_addr) {
    (void)pkt_len;
    const struct iphdr *iph = (const struct iphdr *)pkt;
    uint8_t ip_hdr_len = (iph->ihl << 2);
    const struct udphdr *udph = (const struct udphdr *)(pkt + ip_hdr_len);
    uint16_t udp_len = ntohs(udph->len);

    if (udp_len < sizeof(struct udphdr)) {
        return 0;
    }

    /* Pseudo-header on stack */
    uint8_t pseudo[12] = {0};
    memcpy(pseudo, &src_addr, 4);
    memcpy(pseudo + 4, &dst_addr, 4);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_UDP;
    uint16_t net_len = htons(udp_len);
    memcpy(pseudo + 10, &net_len, 2);

    uint32_t sum = 0;
    sum = inet_checksum((const uint16_t *)pseudo, sizeof(pseudo), sum);

    const uint8_t *udp_start = pkt + ip_hdr_len;
    struct udphdr *udp_tmp = (struct udphdr *)(uintptr_t)udp_start;
    uint16_t orig_csum = udp_tmp->check;
    udp_tmp->check = 0;

    sum = inet_checksum((const uint16_t *)udp_start, udp_len, sum);
    udp_tmp->check = orig_csum;

    return (uint16_t)~sum == 0 ? 0xFFFF : (uint16_t)~sum;
}

/* Compute UDP checksum for IPv6 */
uint16_t turbo_udp6_checksum(const uint8_t *pkt, size_t pkt_len,
                             const uint8_t *src_addr, const uint8_t *dst_addr) {
    (void)pkt_len;
    const struct udphdr *udph = (const struct udphdr *)(pkt + sizeof(struct ip6_hdr));
    uint16_t udp_len = ntohs(udph->len);

    if (udp_len < sizeof(struct udphdr)) {
        return 0;
    }

    /* IPv6 pseudo-header */
    struct ipv6_pseudo_hdr pseudo = {0};
    memcpy(pseudo.src_addr, src_addr, 16);
    memcpy(pseudo.dst_addr, dst_addr, 16);
    pseudo.upper_len = htonl(udp_len);
    pseudo.next_hdr = IPPROTO_UDP;

    uint32_t sum = 0;
    sum = inet_checksum((const uint16_t *)&pseudo, sizeof(pseudo), sum);

    const uint8_t *udp_start = pkt + sizeof(struct ip6_hdr);
    struct udphdr *udp_tmp = (struct udphdr *)(uintptr_t)udp_start;
    uint16_t orig_csum = udp_tmp->check;
    udp_tmp->check = 0;

    sum = inet_checksum((const uint16_t *)udp_start, udp_len, sum);
    udp_tmp->check = orig_csum;

    return (uint16_t)~sum == 0 ? 0xFFFF : (uint16_t)~sum;
}

/* Helper: recalculate UDP checksum for IPv4 packet */
static int udp4_recalc_csum(struct udphdr *udph, const struct iphdr *iph) {
    uint32_t sum = 0;

    /* Pseudo-header */
    uint8_t pseudo[12] = {0};
    memcpy(pseudo, &iph->saddr, 4);
    memcpy(pseudo + 4, &iph->daddr, 4);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_UDP;
    memcpy(pseudo + 10, &udph->len, 2);
    sum = inet_checksum((const uint16_t *)pseudo, sizeof(pseudo), sum);

    /* UDP header + payload */
    udph->check = 0;
    sum = inet_checksum((const uint16_t *)udph, ntohs(udph->len), sum);
    udph->check = (uint16_t)~sum;
    if (udph->check == 0) udph->check = 0xFFFF;

    return 0;
}

/* Helper: recalculate UDP checksum for IPv6 packet */
static int udp6_recalc_csum(struct udphdr *udph, const struct ip6_hdr *iph6) {
    uint32_t sum = 0;

    struct ipv6_pseudo_hdr pseudo = {0};
    memcpy(pseudo.src_addr, &iph6->ip6_src, 16);
    memcpy(pseudo.dst_addr, &iph6->ip6_dst, 16);
    pseudo.upper_len = htonl(ntohs(udph->len));
    pseudo.next_hdr = IPPROTO_UDP;

    sum = inet_checksum((const uint16_t *)&pseudo, sizeof(pseudo), sum);

    udph->check = 0;
    sum = inet_checksum((const uint16_t *)udph, ntohs(udph->len), sum);
    udph->check = (uint16_t)~sum;
    if (udph->check == 0) udph->check = 0xFFFF;

    return 0;
}

/* Rewrite destination (supports IPv4 and IPv6) */
int turbo_switch_rewrite_dst(struct turbo_packet *pkt,
                             const uint8_t *dst_addr, uint16_t dst_port,
                             uint8_t af) {
    if (!pkt || !pkt->data || !dst_addr) {
        return -1;
    }

    if (af == AF_INET4) {
        if (pkt->len < 28) return -1;

        struct iphdr *iph = (struct iphdr *)pkt->data;
        if (iph->version != 4) return -1;

        uint8_t ip_hdr_len = (iph->ihl << 2);
        if (pkt->len < (size_t)ip_hdr_len + sizeof(struct udphdr)) return -1;

        struct udphdr *udph = (struct udphdr *)((uint8_t *)pkt->data + ip_hdr_len);

        /* Rewrite destination */
        memcpy(&iph->daddr, dst_addr, 4);
        udph->dest = dst_port;

        /* Recalculate IP checksum */
        iph->check = 0;
        iph->check = inet_checksum((const uint16_t *)pkt->data, ip_hdr_len, 0);

        /* Recalculate UDP checksum */
        udp4_recalc_csum(udph, iph);

        return 0;

    } else if (af == AF_INET6) {
        if (pkt->len < sizeof(struct ip6_hdr) + sizeof(struct udphdr)) return -1;

        struct ip6_hdr *iph6 = (struct ip6_hdr *)pkt->data;
        if ((iph6->ip6_vfc >> 4) != 6) return -1;

        struct udphdr *udph = (struct udphdr *)((uint8_t *)pkt->data + sizeof(struct ip6_hdr));

        /* Rewrite destination */
        memcpy(&iph6->ip6_dst, dst_addr, 16);
        udph->dest = dst_port;

        /* IPv6 has no header checksum, just UDP */
        udp6_recalc_csum(udph, iph6);

        return 0;
    }

    return -1;
}

/* Rewrite source (supports IPv4 and IPv6) */
int turbo_switch_rewrite_src(struct turbo_packet *pkt,
                             const uint8_t *src_addr, uint16_t src_port,
                             uint8_t af) {
    if (!pkt || !pkt->data || !src_addr) {
        return -1;
    }

    if (af == AF_INET4) {
        if (pkt->len < 28) return -1;

        struct iphdr *iph = (struct iphdr *)pkt->data;
        if (iph->version != 4) return -1;

        uint8_t ip_hdr_len = (iph->ihl << 2);
        if (pkt->len < (size_t)ip_hdr_len + sizeof(struct udphdr)) return -1;

        struct udphdr *udph = (struct udphdr *)((uint8_t *)pkt->data + ip_hdr_len);

        memcpy(&iph->saddr, src_addr, 4);
        udph->source = src_port;

        iph->check = 0;
        iph->check = inet_checksum((const uint16_t *)pkt->data, ip_hdr_len, 0);
        udp4_recalc_csum(udph, iph);

        return 0;

    } else if (af == AF_INET6) {
        if (pkt->len < sizeof(struct ip6_hdr) + sizeof(struct udphdr)) return -1;

        struct ip6_hdr *iph6 = (struct ip6_hdr *)pkt->data;
        if ((iph6->ip6_vfc >> 4) != 6) return -1;

        struct udphdr *udph = (struct udphdr *)((uint8_t *)pkt->data + sizeof(struct ip6_hdr));

        memcpy(&iph6->ip6_src, src_addr, 16);
        udph->source = src_port;

        udp6_recalc_csum(udph, iph6);

        return 0;
    }

    return -1;
}

/* Broadcast: clone + rewrite + batch into tx_array */
int turbo_switch_broadcast(struct turbo_netif *netif,
                           struct turbo_packet *pkt,
                           const uint8_t *dst_addrs,
                           const uint16_t *dst_ports,
                           int num_dsts,
                           uint8_t af,
                           struct turbo_packet **tx_array,
                           int tx_array_size) {
    if (!netif || !pkt || !dst_addrs || !dst_ports || !tx_array || num_dsts <= 0) {
        return 0;
    }

    int prepared = 0;

    for (int i = 0; i < num_dsts && prepared < tx_array_size; i++) {
        struct turbo_packet *cloned = netif->ops->clone_pkt(netif, pkt);
        if (!cloned) {
            continue;
        }

        const uint8_t *addr = dst_addrs + (i * TURBO_MAX_ADDR_LEN);
        if (turbo_switch_rewrite_dst(cloned, addr, dst_ports[i], af) != 0) {
            netif->ops->free_pkt(netif, cloned);
            continue;
        }

        tx_array[prepared++] = cloned;
    }

    return prepared;
}

/* Forward single packet */
int turbo_switch_forward_one(struct turbo_netif *netif,
                             struct turbo_packet *pkt,
                             const uint8_t *dst_addr, uint16_t dst_port,
                             uint8_t af) {
    if (!netif || !pkt || !dst_addr) {
        return -1;
    }

    struct turbo_packet *cloned = netif->ops->clone_pkt(netif, pkt);
    if (!cloned) {
        return -1;
    }

    if (turbo_switch_rewrite_dst(cloned, dst_addr, dst_port, af) != 0) {
        netif->ops->free_pkt(netif, cloned);
        return -1;
    }

    struct turbo_packet *tx_pkts[1] = {cloned};
    uint16_t sent = netif->ops->tx_burst(netif, tx_pkts, 1);

    if (sent == 0) {
        netif->ops->free_pkt(netif, cloned);
        return -1;
    }

    return 0;
}
