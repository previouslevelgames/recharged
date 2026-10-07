#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Actual native metadata for an original MEM logical region. These functions
// provide representation/lifetime only; original MEM C owns every decision.
void* ChargedCreateMEMExpHeapMetadata(void* source, uint32_t bytes);
void* ChargedMEMHeapAddress(const void* heap);
void ChargedValidateMEMHeap(const void* heap);
void* ChargedInitMEMBlock(void* logicalHeader);
void* ChargedMEMBlockAddress(const void* block);
void* ChargedLookupMEMBlock(const void* heap, void* logicalHeader);
void ChargedValidateMEMFree(const void* heap, const void* block);
void ChargedValidateMEMDestroy(const void* heap);
void ChargedRetireMEMHeap(const void* heap);
#ifdef __cplusplus
}
#endif
