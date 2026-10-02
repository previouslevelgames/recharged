#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "NL/gl/glMemory.h"

namespace mscharged
{
namespace { GraphicsCacheInvalidator cache_invalidator = nullptr; }
void SetGraphicsCacheInvalidator(GraphicsCacheInvalidator invalidator)
{ cache_invalidator = invalidator; }

void InvalidateGraphicsCaches()
{
    if (!cache_invalidator)
        MissingStartupService("glplatFrameAllocNextFrame", "Install a real GX cache invalidator before advancing graphics frames");
    cache_invalidator();
}
}

// The decomp declares this virtual but currently supplies no base definition.
// Real GLX pools override it. Keep calls through another derived type explicit.
bool GLResourcePool::GetPoolMemoryInfo(unsigned long, const char**, unsigned long*,
    unsigned long*, unsigned long*, const char**)
{
    mscharged::MissingStartupService("GLResourcePool::GetPoolMemoryInfo", "This resource-pool implementation has no memory-info provider");
}
