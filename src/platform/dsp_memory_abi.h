#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Exact live pinned SDK span, including explicitly retained static mappings.
// No pointer truncation, guessed alias or transient wire token fallback.
uint32_t ChargedDSPTaskMemoryWord(const void* address, uint32_t bytes, int device_writes);
#ifdef __cplusplus
}
#endif
