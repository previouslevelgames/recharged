#pragma once

#include <cstddef>
#include <cstdint>

using EventOwnerHandle = std::uintptr_t;

// The original union aliases use Wii numeric flag bits. Native bitfield order
// follows the compiler's byte order; these declarations retain those same bits.
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define MSCHARGED_EVENT_NATIVE_LITTLE_ENDIAN 1
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define MSCHARGED_EVENT_NATIVE_LITTLE_ENDIAN 0
#else
#error Native event bitfield transport requires a declared compiler byte order
#endif

#define MSCHARGED_EVENT_ENTRY_OFFSET(Type) offsetof(Type, entry)
