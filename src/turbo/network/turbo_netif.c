#include "turbo_netif.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Backend operations are defined in turbo_dpdk.c or turbo_af_xdp.c */
extern const struct turbo_netif_ops turbo_dpdk_ops;
extern const struct turbo_netif_ops turbo_afxdp_ops;

struct turbo_netif* turbo_netif_init(const char *ifname, uint16_t port) {
    struct turbo_netif *netif;
    const struct turbo_netif_ops *ops;
    int ret;

    netif = malloc(sizeof(*netif));
    if (!netif) {
        return NULL;
    }

    memset(netif, 0, sizeof(*netif));

    /* Select backend based on compile-time configuration */
#ifdef TURN_USE_DPDK
    ops = &turbo_dpdk_ops;
#elif defined(TURN_USE_AFXDP)
    ops = &turbo_afxdp_ops;
#else
    free(netif);
    return NULL;
#endif

    netif->ops = ops;

    /* Use default interface name if not provided */
    if (ifname && ifname[0]) {
        strncpy(netif->ifname, ifname, sizeof(netif->ifname) - 1);
    } else {
#ifdef TURN_USE_DPDK
        strncpy(netif->ifname, "0", sizeof(netif->ifname) - 1);
#else
        strncpy(netif->ifname, "eth0", sizeof(netif->ifname) - 1);
#endif
    }

    netif->port = port;

    ret = ops->init(netif, netif->ifname, port);
    if (ret != 0) {
        free(netif);
        return NULL;
    }

    return netif;
}

void turbo_netif_cleanup(struct turbo_netif *netif) {
    if (!netif) {
        return;
    }

    if (netif->ops && netif->ops->cleanup) {
        netif->ops->cleanup(netif);
    }

    free(netif);
}
