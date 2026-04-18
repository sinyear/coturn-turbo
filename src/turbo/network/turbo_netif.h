#ifndef TURBO_NETIF_H
#define TURBO_NETIF_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
struct turbo_netif;
struct turbo_packet;

/**
 * Network interface operations structure
 * Defines the abstract interface for network backends (DPDK, AF_XDP, etc.)
 */
struct turbo_netif_ops {
    /**
     * Initialize the network interface
     * @param netif Pointer to network interface structure
     * @param ifname Interface name (e.g., "eth0", "dpdk0")
     * @param port UDP port to bind to
     * @return 0 on success, negative error code on failure
     */
    int (*init)(struct turbo_netif *netif, const char *ifname, uint16_t port);

    /**
     * Cleanup the network interface
     * @param netif Pointer to network interface structure
     */
    void (*cleanup)(struct turbo_netif *netif);

    /**
     * Receive a burst of packets
     * @param netif Pointer to network interface structure
     * @param pkts Array to store received packets
     * @param nb_pkts Maximum number of packets to receive
     * @return Number of packets actually received
     */
    uint16_t (*rx_burst)(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts);

    /**
     * Transmit a burst of packets
     * @param netif Pointer to network interface structure
     * @param pkts Array of packets to transmit
     * @param nb_pkts Number of packets to transmit
     * @return Number of packets actually transmitted
     */
    uint16_t (*tx_burst)(struct turbo_netif *netif, struct turbo_packet **pkts, uint16_t nb_pkts);

    /**
     * Allocate a packet buffer
     * @param netif Pointer to network interface structure
     * @param size Size of the packet buffer to allocate
     * @return Pointer to allocated packet, NULL on failure
     */
    struct turbo_packet* (*alloc_pkt)(struct turbo_netif *netif, size_t size);

    /**
     * Free a packet buffer
     * @param netif Pointer to network interface structure
     * @param pkt Pointer to packet to free
     */
    void (*free_pkt)(struct turbo_netif *netif, struct turbo_packet *pkt);

    /**
     * Clone a packet (zero-copy if possible)
     * @param netif Pointer to network interface structure
     * @param pkt Original packet to clone
     * @return Pointer to cloned packet, NULL on failure
     */
    struct turbo_packet* (*clone_pkt)(struct turbo_netif *netif, struct turbo_packet *pkt);
};

/**
 * Network interface structure
 */
struct turbo_netif {
    const struct turbo_netif_ops *ops;  /* Operations */
    void *priv;                         /* Private backend data */
    uint16_t port;                      /* UDP port */
    char ifname[32];                    /* Interface name */
};

/**
 * Packet structure
 */
struct turbo_packet {
    void *data;         /* Packet data buffer */
    size_t len;         /* Packet length */
    size_t buf_len;     /* Buffer length */
    void *priv;         /* Private backend data */
};

/* Backend types */
#define TURBO_BACKEND_DPDK    1
#define TURBO_BACKEND_AFXDP   2

/**
 * Initialize network interface (backend-agnostic)
 * @param ifname Interface name (e.g., "eth0")
 * @param port UDP port
 * @return Pointer to initialized turbo_netif, NULL on failure
 */
struct turbo_netif* turbo_netif_init(const char *ifname, uint16_t port);

/**
 * Cleanup network interface
 * @param netif Network interface to cleanup
 */
void turbo_netif_cleanup(struct turbo_netif *netif);

#endif /* TURBO_NETIF_H */