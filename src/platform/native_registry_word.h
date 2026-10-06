#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mscharged::platform {

// Some original registry values use a pointer-shaped cell for a scalar/hash
// word. This is data transport, not a pointer or physical-address translation.
// Native decoded Wii32 cells are zero extended; live pointers stay full width.
inline std::uint32_t ReadWii32RegistryWord(const void* encoded_word) {
    const auto bits = reinterpret_cast<std::uintptr_t>(encoded_word);
    if (bits > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Native registry scalar exceeds its original Wii32 word");
    return static_cast<std::uint32_t>(bits);
}

} // namespace mscharged::platform
