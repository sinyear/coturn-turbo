/*
 * XDP program for AF_XDP packet filtering and direction.
 *
 * This eBPF program runs in the XDP layer and decides whether
 * to pass packets to user-space via AF_XDP or let them continue
 * through the kernel network stack.
 *
 * Compilation:
 *   clang -O2 -target bpf -I/usr/include -c xdp_prog.c -o xdp_prog.o
 *
 * Usage:
 *   ip link set dev eth0 xdp obj xdp_prog.o sec xdp
 *
 * The program uses a BPF_MAP_TYPE_ARRAY to store the target UDP port,
 * allowing runtime configuration via the port_map map.
 * Non-matching packets are passed to the kernel stack (XDP_PASS),
 * ensuring SSH and other services are not affected.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>

/*
 * Byte-order conversion helpers for BPF programs.
 * Inlined here to avoid libbpf version compatibility issues.
 * __bpf_htons/__bpf_ntohs are not stable across libbpf versions;
 * some use bpf_htons/bpf_ntohs, others hide them entirely.
 */
#ifndef __bpf_htons
static __always_inline __u16 __bpf_htons(__u16 val)
{
	return __builtin_bswap16(val);
}
#endif

#ifndef __bpf_ntohs
static __always_inline __u16 __bpf_ntohs(__u16 val)
{
	return __builtin_bswap16(val);
}
#endif

/* XDP action codes */
#ifndef XDP_PASS
#define XDP_PASS 2
#endif
#ifndef XDP_DROP
#define XDP_DROP 1
#endif
#ifndef XDP_REDIRECT
#define XDP_REDIRECT 3
#endif

/* UDP port to capture (configurable via map) */
#define TURN_PORT 3478

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__type(key, __u32);
	__type(value, __u16);
	__uint(max_entries, 1);
} port_map SEC(".maps");

SEC("xdp")
int xdp_filter(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;

	/* Parse Ethernet header */
	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	/* Only process IPv4 */
	if (eth->h_proto != __bpf_htons(ETH_P_IP)) {
		if (eth->h_proto != __bpf_htons(ETH_P_IPV6))
			return XDP_PASS;

		/* IPv6 path */
		struct ipv6hdr *ip6h = (void *)(eth + 1);
		if ((void *)(ip6h + 1) > data_end)
			return XDP_PASS;

		if (ip6h->nexthdr != IPPROTO_UDP)
			return XDP_PASS;

		struct udphdr *udph = (void *)(ip6h + 1);
		if ((void *)(udph + 1) > data_end)
			return XDP_PASS;

		__u16 dst_port = __bpf_ntohs(udph->dest);
		__u32 port_key = 0;
		__u16 *target_port = bpf_map_lookup_elem(&port_map, &port_key);
		if (target_port && dst_port == *target_port)
			return XDP_REDIRECT;

		return XDP_PASS;
	}

	/* Parse IP header */
	struct iphdr *iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end)
		return XDP_PASS;

	/* Only process UDP packets */
	if (iph->protocol != IPPROTO_UDP)
		return XDP_PASS;

	/* Parse UDP header */
	struct udphdr *udph = (void *)iph + (iph->ihl << 2);
	if ((void *)(udph + 1) > data_end)
		return XDP_PASS;

	/* Check destination port */
	__u16 dst_port = __bpf_ntohs(udph->dest);

	/* Check against configured port via map */
	__u32 port_key = 0;
	__u16 *target_port = bpf_map_lookup_elem(&port_map, &port_key);
	if (target_port && dst_port == *target_port) {
		/* Packet belongs to TURN port - pass to AF_XDP */
		return XDP_REDIRECT;
	}

	/* Default: let other packets pass through kernel stack */
	return XDP_PASS;
}

/* Simple version without maps - hardcoded port (fallback) */
SEC("xdp_simple")
int xdp_filter_simple(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;

	struct ethhdr *eth = data;
	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	if (eth->h_proto != __bpf_htons(ETH_P_IP)) {
		if (eth->h_proto != __bpf_htons(ETH_P_IPV6))
			return XDP_PASS;

		/* IPv6 */
		struct ipv6hdr *ip6h = (void *)(eth + 1);
		if ((void *)(ip6h + 1) > data_end)
			return XDP_PASS;
		if (ip6h->nexthdr != IPPROTO_UDP)
			return XDP_PASS;
		struct udphdr *udph = (void *)(ip6h + 1);
		if ((void *)(udph + 1) > data_end)
			return XDP_PASS;
		if (__bpf_ntohs(udph->dest) == TURN_PORT)
			return XDP_REDIRECT;
		return XDP_PASS;
	}

	struct iphdr *iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end)
		return XDP_PASS;

	if (iph->protocol != IPPROTO_UDP)
		return XDP_PASS;

	struct udphdr *udph = (void *)iph + (iph->ihl << 2);
	if ((void *)(udph + 1) > data_end)
		return XDP_PASS;

	if (__bpf_ntohs(udph->dest) == TURN_PORT)
		return XDP_REDIRECT;

	return XDP_PASS;
}

char __license[] SEC("license") = "GPL";
