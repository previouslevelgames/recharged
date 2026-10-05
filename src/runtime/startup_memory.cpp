#include "runtime/startup.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include <dolphin/os.h>

namespace mscharged
{
void InitializeStartupOS() { OSInit(); ResetStartupMemory(); }

void ResetStartupMemory()
{
    // The prototype owns no surviving game objects; clear pointers before Aurora
    // frees their arenas. Full game shutdown must destroy those objects first.
    StandardAllocator = {};
    VirtualAllocator = {};
    CurrentAllocator = &StandardAllocator;
    for (auto& allocator : AllocatorStack) allocator = nullptr;
    AllocatorStack[0] = &StandardAllocator;
    AllocatorStackDepth = 1;
    gMemoryInitialized = 0;
}

std::string StartupMemorySummary()
{
    if (!gMemoryInitialized || !StandardAllocator.m_memory || !VirtualAllocator.m_memory)
        return "Original game memory initialization is incomplete.";
    return "Original nlInitMemory completed; MEM1 game arena: "
        + std::to_string(StandardAllocator.m_memory_size) + " bytes; MEM2 game arena: "
        + std::to_string(VirtualAllocator.m_memory_size) + " bytes; reserved SDK heap initialized.";
}
}
