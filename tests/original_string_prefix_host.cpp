// Retained CPU fixture arenas precede untouched original string constructors.
// This is not production module initialization or a CRT teardown gate.
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include <cstddef>

namespace {
alignas(32) std::byte standard_memory[2 * 1024 * 1024];
alignas(32) std::byte virtual_memory[2 * 1024 * 1024];
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor(101))) void InitializeFixtureArenas()
{
    StandardAllocator.Initialize(standard_memory, sizeof(standard_memory));
    VirtualAllocator.Initialize(virtual_memory, sizeof(virtual_memory));
    gMemoryInitialized = 1;
}
#else
#error This fixture requires an explicitly qualified early-constructor mechanism.
#endif
}
