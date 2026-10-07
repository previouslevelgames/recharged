#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void* ChargedAllocRFLTemp(void* heap,uint32_t requestedBytes,int32_t alignment);
void ChargedFreeRFLTemp(void* heap,void* block);
void* ChargedDestroyRFLTempHeap(void* heap);
#ifdef __cplusplus
}
#endif
