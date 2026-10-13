#ifndef CHARGED_NET_SO_HOST_H
#define CHARGED_NET_SO_HOST_H

// Host queries for the SO backend that need libc headers so.c cannot include:
// <stdlib.h> defines wchar_t as int, the RVL <revolution/types.h> defines it
// as unsigned short, and C rejects the second typedef. This TU sees only libc.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// never 0: falls back to 127.0.0.1 when no interface qualifies.
uint32_t so_host_find_ipv4(void);

#ifdef __cplusplus
}
#endif

#endif // CHARGED_NET_SO_HOST_H
