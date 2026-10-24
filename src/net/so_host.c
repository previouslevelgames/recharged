#include "so_host.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static uint32_t sockaddr_ipv4(const struct sockaddr* sa)
{
    return sa != NULL && sa->sa_family == AF_INET
        ? ((const struct sockaddr_in*)sa)->sin_addr.s_addr
        : 0;
}

// RECHARGED_NET_IFACE (e.g. "enp3s0") forces one interface when several
// qualify (Docker bridges, VPNs, wifi + ethernet)
void so_host_find_ipv4_info(struct so_host_ipv4* out)
{
    const char* wanted = getenv("RECHARGED_NET_IFACE");
    if (wanted == NULL)
        wanted = getenv("MSCHARGED_NET_IFACE"); /* before the rename (#64) */
    if (wanted != NULL && wanted[0] == '\0')
        wanted = NULL;

    *out = (struct so_host_ipv4){ 0 };
    struct ifaddrs* list;
    if (getifaddrs(&list) == 0)
    {
        for (struct ifaddrs* ifa = list; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if ((ifa->ifa_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
                continue;
            if (wanted != NULL)
            {
                if (strcmp(ifa->ifa_name, wanted) != 0)
                    continue;
            }
            else if (ifa->ifa_flags & IFF_LOOPBACK)
            {
                continue;
            }
            out->addr = sockaddr_ipv4(ifa->ifa_addr);
            out->netmask = sockaddr_ipv4(ifa->ifa_netmask);
            // Point-to-point links (VPNs) have no broadcast address there.
            if (ifa->ifa_flags & IFF_BROADCAST)
                out->broadcast = sockaddr_ipv4(ifa->ifa_broadaddr);
            break;
        }
        freeifaddrs(list);
    }

    if (out->addr == 0)
    {
        out->addr = htonl(INADDR_LOOPBACK);
        out->netmask = htonl(0xFF000000u);
    }
    if (out->broadcast == 0)
        out->broadcast = out->addr | ~out->netmask;
}

uint32_t so_host_find_ipv4(void)
{
    struct so_host_ipv4 info;
    so_host_find_ipv4_info(&info);
    return info.addr;
}
