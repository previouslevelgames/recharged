#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The original ARC parser owns all search/traversal and file decisions.
// These operations only read bounded original serialized storage/address it.
void* ChargedARCHeader(const void* archive, size_t native_alignment);
uint32_t ChargedReadARCWord(const void* word);
void* ChargedARCByteOffset(const void* pointer, intptr_t offset);
void ChargedValidateARCLayout(const void* archive, int32_t fst_start,
    int32_t fst_bytes, int32_t file_start, size_t native_fst_alignment);

#ifdef __cplusplus
}
#endif
