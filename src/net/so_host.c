#include "so_host.h"

#if defined(__SWITCH__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <switch.h>

// Horizon lists no network interfaces: the console's current address comes
// from its network service.
uint32_t so_host_find_ipv4(void)
{
    u32 address = 0;
    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User)))
    {
        if (R_FAILED(nifmGetCurrentIpAddress(&address)))
            address = 0;
        nifmExit();
    }
    return address != 0 ? address : htonl(INADDR_LOOPBACK);
}
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

// RECHARGED_NET_IFACE (e.g. "enp3s0") forces one interface when several
// qualify (Docker bridges, VPNs, wifi + ethernet)
uint32_t so_host_find_ipv4(void)
{
    const char* wanted = getenv("RECHARGED_NET_IFACE");
    if (wanted == NULL)
        wanted = getenv("MSCHARGED_NET_IFACE"); /* before the rename (#64) */
    if (wanted != NULL && wanted[0] == '\0')
        wanted = NULL;

    uint32_t found = 0;
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
            found = ((const struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
            break;
        }
        freeifaddrs(list);
    }

    if (found == 0)
        found = htonl(INADDR_LOOPBACK);
    return found;
}
#endif
