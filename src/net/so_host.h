#ifndef CHARGED_NET_SO_HOST_H
#define CHARGED_NET_SO_HOST_H

// Host queries for the SO backend that need libc headers so.c cannot include:
// <stdlib.h> defines wchar_t as int, the RVL <revolution/types.h> defines it
// as unsigned short, and C rejects the second typedef. This TU sees only libc.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The IPv4 configuration of the interface the backend uses, in network byte
// order.
struct so_host_ipv4
{
    uint32_t addr;
    uint32_t netmask;
    uint32_t broadcast;
};

// Never empty: falls back to 127.0.0.1/8 when no interface qualifies. An
// interface without a broadcast address gets addr | ~netmask.
void so_host_find_ipv4_info(struct so_host_ipv4* out);
// The address alone; never 0.
uint32_t so_host_find_ipv4(void);

#ifdef __cplusplus
}
#endif

#endif // CHARGED_NET_SO_HOST_H
