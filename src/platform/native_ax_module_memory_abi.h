#pragma once
#include "platform/ax_storage_abi.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ChargedAXModuleArenaSnapshot {
    uintptr_t standard_address, virtual_address;
    uint32_t standard_bytes, virtual_bytes;
    uint32_t memory_initialized;
} ChargedAXModuleArenaSnapshot;
typedef void (*ChargedAXModuleArenaObserver)(ChargedAXModuleArenaSnapshot*);
// Real host loader endpoint, armed before dlopen. The platform-only module
// constructor submits genuine source static storage before arena capture.
void ChargedNativeAXReserveModuleStorage(const ChargedAXStorage* storage,
    uint32_t count, ChargedAXStorage cpu_task,
    ChargedAXModuleArenaSnapshot before, ChargedAXModuleArenaObserver observe);
#ifdef __cplusplus
}
#endif
