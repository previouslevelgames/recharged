#include "so_host.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sockaddr_ipv4(const struct sockaddr* sa)
{
    return sa != NULL && sa->sa_family == AF_INET
        ? ((const struct sockaddr_in*)sa)->sin_addr.s_addr
        : 0;
}

// RECHARGED_NET_IFACE names the adapter as Windows shows it (e.g. "Ethernet")
// or by its GUID.
static int adapter_matches(const IP_ADAPTER_ADDRESSES* adapter, const char* wanted)
{
    char friendly[256];
    if (WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1, friendly, (int)sizeof(friendly), NULL, NULL) > 0
        && strcmp(friendly, wanted) == 0)
        return 1;
    return strcmp(adapter->AdapterName, wanted) == 0;
}

// Windows keeps virtual adapters up (WSL and Hyper-V switches, VPNs), so an
// adapter with a default gateway is preferred; the first other one is the
// fallback. RECHARGED_NET_IFACE forces one adapter, as on POSIX.
void so_host_find_ipv4_info(struct so_host_ipv4* out)
{
    const char* wanted = getenv("RECHARGED_NET_IFACE");
    if (wanted == NULL)
        wanted = getenv("MSCHARGED_NET_IFACE"); /* before the rename (#64) */
    if (wanted != NULL && wanted[0] == '\0')
        wanted = NULL;

    *out = (struct so_host_ipv4){ 0 };
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER
        | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES* list = NULL;
    ULONG error = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && error == ERROR_BUFFER_OVERFLOW; attempt++)
    {
        free(list);
        list = malloc(size);
        if (list == NULL)
            break;
        error = GetAdaptersAddresses(AF_INET, flags, NULL, list, &size);
    }

    if (list != NULL && error == NO_ERROR)
    {
        const IP_ADAPTER_UNICAST_ADDRESS* chosen = NULL;
        for (const IP_ADAPTER_ADDRESSES* adapter = list; adapter != NULL; adapter = adapter->Next)
        {
            if (adapter->OperStatus != IfOperStatusUp)
                continue;
            if (wanted != NULL)
            {
                if (!adapter_matches(adapter, wanted))
                    continue;
            }
            else if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            {
                continue;
            }
            const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
            while (unicast != NULL && sockaddr_ipv4(unicast->Address.lpSockaddr) == 0)
                unicast = unicast->Next;
            if (unicast == NULL)
                continue;
            if (chosen == NULL)
                chosen = unicast;
            if (wanted != NULL || adapter->FirstGatewayAddress != NULL)
            {
                chosen = unicast;
                break;
            }
        }
        if (chosen != NULL)
        {
            out->addr = sockaddr_ipv4(chosen->Address.lpSockaddr);
            const ULONG prefix = chosen->OnLinkPrefixLength;
            out->netmask = prefix == 0 ? 0 : htonl(prefix >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix));
        }
    }
    free(list);

    // Windows reports no broadcast address; it is derived below, as POSIX
    // does for point-to-point links.
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
