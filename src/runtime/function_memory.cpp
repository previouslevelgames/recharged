// Native storage for the NL callback functors used by the file layer. The
// original fixed pools and their state-stack API are not enabled in this subset.
#include "NL/nlFunctionMemory.h"
#include "NL/nlMemory.h"
#include <cstddef>

void* AllocateFunctionMemory(unsigned long size)
{
    return nlMalloc(size, alignof(std::max_align_t), false);
}

void FreeFunctionMemory(void* entry, unsigned long)
{
    nlFree(entry);
}
