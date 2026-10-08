#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace mscharged::platform {

// AXFX's native hook uses size_t; the unchanged source allocator uses unsigned
// long. Reject an unrepresentable request before a narrowing conversion.
inline unsigned long NativeAudioAllocationSize(std::size_t size) {
    if (size > std::numeric_limits<unsigned long>::max())
        throw std::length_error("Native audio allocation exceeds the source size carrier");
    return static_cast<unsigned long>(size);
}

} // namespace mscharged::platform
