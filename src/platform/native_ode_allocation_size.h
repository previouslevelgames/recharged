#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace mscharged::platform {

// Native ODE uses size_t; the unchanged source NL allocator uses unsigned long.
// Validate the actual host carrier before any source allocation or copy request.
inline unsigned long NativeODEAllocationSize(std::size_t size) {
    if (size > std::numeric_limits<unsigned long>::max())
        throw std::length_error("Native ODE allocation exceeds the source size carrier");
    return static_cast<unsigned long>(size);
}

} // namespace mscharged::platform
