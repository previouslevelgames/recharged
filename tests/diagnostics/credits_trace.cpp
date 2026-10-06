// Transparent fixture observer: records genuine source header requests and
// delegates exactly once with identical size/type/pool; no source budget fix.
#include "Game/BaseGameSceneManager.h"
#include "NL/gl/glMemory.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlBundleFile.h"
#include <cstdio>
extern "C" void* RealAllocation(unsigned long, eGLMemory, void*)
    asm("__real__Z15glResourceAllocm9eGLMemoryPv");
extern "C" void* TraceAllocation(unsigned long, eGLMemory, void*)
    asm("__wrap__Z15glResourceAllocm9eGLMemoryPv");
extern "C" void* TraceAllocation(unsigned long bytes, eGLMemory type, void* resource)
{
    auto* pool = static_cast<GLResourcePool*>(resource);
    static bool reported;
    if (!reported && pool == GetFEResourcePool() && type == GLM_Header) {
        reported = true;
        const char *name, *other;
        unsigned long total, free, peak;
        pool->GetPoolMemoryInfo(0, &name, &total, &free, &peak, &other);
        std::fprintf(stderr, "231 source FE header request=%lu used=%lu capacity=%lu PlatTexture=%zu GXTexObj=%zu GXTlutObj=%zu\n",
                     bytes, total-free, total, sizeof(PlatTexture), sizeof(GXTexObj), sizeof(GXTlutObj));
    }
    return RealAllocation(bytes,type,resource);
}
extern "C" bool RealBundleInfo(BundleFile*, unsigned long, BundleFileDirectoryEntry*)
    asm("__real__ZN10BundleFile18GetFileInfoByIndexEmP24BundleFileDirectoryEntry");
extern "C" bool TraceBundleInfo(BundleFile*, unsigned long, BundleFileDirectoryEntry*)
    asm("__wrap__ZN10BundleFile18GetFileInfoByIndexEmP24BundleFileDirectoryEntry");
extern "C" bool TraceBundleInfo(BundleFile* bundle, unsigned long index, BundleFileDirectoryEntry* entry)
{
    if (index == 0)
        std::fprintf(stderr, "231 original BundleFile source count=%u\n", bundle->nNumFiles);
    return RealBundleInfo(bundle,index,entry);
}
