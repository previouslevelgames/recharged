#pragma once
#include <stdint.h>
#include "platform/ax_storage_abi.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ChargedAXOutputMemorySnapshot {
    uintptr_t standard_address, virtual_address;
    uint32_t standard_bytes, virtual_bytes;
    uint32_t standard_allocations, virtual_allocations;
    uint32_t memory_initialized;
} ChargedAXOutputMemorySnapshot;
// Fixture-only loader endpoints. The actual source image and its static arrays
// exist before original eager function-pool constructors capture SDK arenas.
void ChargedAXOutputReserve311(const ChargedAXStorage* storage, uint32_t count,
                              ChargedAXStorage task,
                              ChargedAXOutputMemorySnapshot before);
void ChargedAXOutputRetired311(void);
void charged_ax_output_snapshot(ChargedAXOutputMemorySnapshot* output);
unsigned charged_ax_output_pool_owner_check(void);
#ifdef __cplusplus
}
#endif
