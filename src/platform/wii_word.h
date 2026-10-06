#pragma once

#include <cstdint>
#include <cstring>

namespace mscharged::platform
{
// Copy the original four-byte word from an already-native object. This is not
// a native pointer read or an endian conversion of serialized file bytes.
inline std::uint32_t ReadNativeWord32(const void* source) noexcept
{
    std::uint32_t value;
    std::memcpy(&value, source, sizeof(value));
    return value;
}
}
