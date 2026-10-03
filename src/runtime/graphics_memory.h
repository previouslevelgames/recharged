#pragma once

#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"

namespace mscharged
{
// The memory target also runs without a GPU. Frame advancement requires an
// installed cache invalidator; the scene supplies the real Aurora GX calls.
using GraphicsCacheInvalidator = void (*)();
void SetGraphicsCacheInvalidator(GraphicsCacheInvalidator invalidator);
void InvalidateGraphicsCaches();
// Release registered views/targets before their game pools and frame storage.
void SetGraphicsViewShutdown(void (*shutdown)());
void ShutdownGraphicsViews();

class ScopedGameAllocator
{
    MemoryAllocator* previous_;
public:
    explicit ScopedGameAllocator(MemoryAllocator& allocator)
        : previous_(CurrentAllocator) { CurrentAllocator = &allocator; }
    ~ScopedGameAllocator() { CurrentAllocator = previous_; }
    ScopedGameAllocator(const ScopedGameAllocator&) = delete;
    ScopedGameAllocator& operator=(const ScopedGameAllocator&) = delete;
};
}
