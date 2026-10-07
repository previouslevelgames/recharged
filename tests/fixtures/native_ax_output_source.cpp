#include "native_ax_output_source.h"
#include "original_slot_stride.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
#include "NL/nlFunctionMemory.h"
#include <stdexcept>
#include <cstring>
extern "C" {
#include <revolution/ax.h>
}

namespace {
ChargedAXOutputMemorySnapshot Snapshot() {
    return {reinterpret_cast<uintptr_t>(StandardAllocator.m_memory),
            reinterpret_cast<uintptr_t>(VirtualAllocator.m_memory),
            StandardAllocator.m_memory_size, VirtualAllocator.m_memory_size,
            StandardAllocator.m_allocation_count, VirtualAllocator.m_allocation_count,
            gMemoryInitialized};
}
// This opt-in platform fixture is not admitted to the production game image.
// A loader contract is required before it can be used by the full game: SDK
// arenas already exist, this reservation runs before any source arena capture,
// and a real image lease backs each address until device jobs have drained.
__attribute__((constructor(101))) void ReserveSourceStatics() {
    ChargedAXStorage storage[13];
    storage[0] = ChargedAXGetCommandStorage();
    ChargedAXGetVoiceStorage(storage + 1);
    ChargedAXGetAuxStorage(storage + 3);
    storage[6] = ChargedAXGetCompressorStorage();
    storage[7] = {__AXGetStudio(), sizeof(AXSTUDIO)};
    ChargedAXGetOutputStorage(storage + 8);
    ChargedAXOutputReserve311(storage, 13, ChargedAXGetTaskStorage(), Snapshot());
}
__attribute__((destructor(101))) void ObserveRetirement() {
    ChargedAXOutputRetired311();
}
}

#define EXPORTED extern "C" __attribute__((visibility("default")))
EXPORTED void charged_ax_output_snapshot(ChargedAXOutputMemorySnapshot* output) {
    *output = Snapshot();
}
EXPORTED unsigned charged_ax_output_pool_owner_check() {
    // Exercise the literal source eager-pool ownership without reconstructing
    // initialization or modifying its logical categories/counts/state.
    void* slots[3];
    for (unsigned i = 0; i != 3; ++i) {
        slots[i] = AllocateFunctionMemory(16u << i);
        const auto address = reinterpret_cast<uintptr_t>(slots[i]);
        const auto low = reinterpret_cast<uintptr_t>(VirtualAllocator.m_memory);
        if (!slots[i] || address < low || address >= low + VirtualAllocator.m_memory_size)
            throw std::runtime_error("Actual source function pool escaped captured MEM2");
        std::memset(slots[i], 0x31 + i, 16u << i);
    }
    for (unsigned i = 0; i != 3; ++i) FreeFunctionMemory(slots[i], 16u << i);
    return 3;
}

EXPORTED unsigned charged_ax_output_slot_stride_check() {
    return mscharged::testing::slot_stride::QualifyOriginalSlotStride();
}
