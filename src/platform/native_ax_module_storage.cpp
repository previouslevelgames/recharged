#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE) || !defined(MSCHARGED_NATIVE_AX_MODULE_MEMORY)
#error Actual source AX storage reservation requires its explicit original module profile
#endif
#include "platform/native_ax_module_memory_abi.h"
#include "NL/nlMemory.h"
#include "NL/MemAlloc.h"
extern "C" {
#include <revolution/ax.h>
}
namespace {
void ObserveArenas(ChargedAXModuleArenaSnapshot* result) {
    *result = {reinterpret_cast<uintptr_t>(StandardAllocator.m_memory),
               reinterpret_cast<uintptr_t>(VirtualAllocator.m_memory),
               StandardAllocator.m_memory_size, VirtualAllocator.m_memory_size,
               gMemoryInitialized};
}
// Pure platform/data setup. No game object is constructed, no arena is captured
// here and no source Initialize/AddTasks/AXInit or callback is bypassed.
__attribute__((constructor(101))) void ReserveActualSourceAXStorage() {
#if defined(MSCHARGED_NATIVE_HBM_MODULE_MEMORY)
    constexpr unsigned storage_count = CHARGED_AX_HBM_STORAGE_COUNT;
#else
    constexpr unsigned storage_count = CHARGED_AX_BASE_STORAGE_COUNT;
#endif
    ChargedAXStorage storage[storage_count];
    storage[0] = ChargedAXGetCommandStorage();
    ChargedAXGetVoiceStorage(storage + 1);
    ChargedAXGetAuxStorage(storage + 3);
    storage[6] = ChargedAXGetCompressorStorage();
    storage[7] = {__AXGetStudio(), sizeof(AXSTUDIO)};
    ChargedAXGetOutputStorage(storage + 8);
#if defined(MSCHARGED_NATIVE_HBM_MODULE_MEMORY)
    storage[CHARGED_AX_BASE_STORAGE_COUNT] = ChargedHBMGetZeroStorage();
#endif
    ChargedAXModuleArenaSnapshot before{};
    ObserveArenas(&before);
    ChargedNativeAXReserveModuleStorage(storage, storage_count, ChargedAXGetTaskStorage(),
                                       before, ObserveArenas);
}
}
