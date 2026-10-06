// Bounded private CPU fixture memory before untouched original StringBlock and
// texture tweak constructors. This is not production Aurora/module initialization.
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include <cstddef>

namespace {
alignas(32) std::byte standard_memory[16 * 1024 * 1024];
alignas(32) std::byte virtual_memory[32 * 1024 * 1024];
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor(101))) void FixtureHostMemory()
{
    StandardAllocator.Initialize(standard_memory, sizeof(standard_memory));
    VirtualAllocator.Initialize(virtual_memory, sizeof(virtual_memory));
    gMemoryInitialized = 1;
}
#else
#error Private early-arena CPU fixture requires an explicitly qualified constructor mechanism.
#endif
}
