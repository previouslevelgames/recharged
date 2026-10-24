// Horizon has no dynamic loader: no module is ever listed (see dlfcn.h here).
#pragma once
#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#define ElfW(type) Elf64_##type

#ifdef __cplusplus
extern "C" {
#endif

struct link_map {
    ElfW(Addr) l_addr;
    char* l_name;
    ElfW(Dyn)* l_ld;
    struct link_map *l_next, *l_prev;
};

struct dl_phdr_info {
    ElfW(Addr) dlpi_addr;
    const char* dlpi_name;
    const ElfW(Phdr)* dlpi_phdr;
    ElfW(Half) dlpi_phnum;
};

static inline int dl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data)
{
    (void)callback;
    (void)data;
    return 0;
}

#ifdef __cplusplus
}
#endif
