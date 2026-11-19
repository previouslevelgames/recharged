// Horizon has no dynamic loading. The program links its game modules as
// static images (cmake/StaticGameImages.cmake), which the module loader finds
// first; anything else it would load at run time fails here, as dlopen fails
// for a missing file.
#pragma once
#include <stddef.h>

#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_NOLOAD 0x4
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DI_LINKMAP 2

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
} Dl_info;

static inline void* dlopen(const char* path, int flags) { (void)path; (void)flags; return NULL; }
static inline const char* dlerror(void) { return "Dynamic loading is not available on the Nintendo Switch"; }
static inline void* dlsym(void* handle, const char* name) { (void)handle; (void)name; return NULL; }
static inline int dlclose(void* handle) { (void)handle; return -1; }
static inline int dladdr(const void* address, Dl_info* info) { (void)address; (void)info; return 0; }
static inline int dlinfo(void* handle, int request, void* info) { (void)handle; (void)request; (void)info; return -1; }

#ifdef __cplusplus
}
#endif
