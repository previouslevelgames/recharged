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

#include "platform/game_allocation_ownership.h"

namespace mscharged::platform
{
struct NativeARCFileSpan
{
    const void* base;
    std::size_t bytes;
    GameCompletedSpan archive;
};

// Data bounds only: the logical completed read must itself be the raw ARC.
// Match one exact, nonempty FST file start in that live Wii-serialized read.
// Interior/nested files, ambiguity and retired/uncompleted owners reject and
// clear result. This query grants no lifetime; the caller keeps its real owner
// quiescent throughout the query and any subsequent resource use.
bool FindNativeARCFileSpan(const void* file, NativeARCFileSpan& result);
}
#endif
