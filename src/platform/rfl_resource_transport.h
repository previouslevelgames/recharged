#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Source cached-size and actual completed owner bound serialized reads. */
uint16_t ChargedReadRFLCachedHalf(const void* cache, uint32_t cache_size, const void* word);
uint32_t ChargedReadRFLCachedWord(const void* cache, uint32_t cache_size, const void* word);
void* ChargedRFLCachedAddress(const void* cache, uint32_t cache_size,
    uint32_t offset, size_t native_alignment);
void ChargedCopyRFLCachedBytes(void* destination, const void* cache,
    uint32_t cache_size, uint32_t offset, uint32_t bytes);

/* NAND buffers require their own true completed serialized read publication. */
uint16_t ChargedReadRFLNANDHalf(const void* word);
uint32_t ChargedReadRFLNANDWord(const void* word);

#ifdef __cplusplus
}
#endif
