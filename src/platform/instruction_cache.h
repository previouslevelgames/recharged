#pragma once
#include <cstdint>

namespace mscharged::platform {
// The current native images are immutable, OS-loader-published machine code.
// This serial observes real compiler/hardware ordering at ICFlashInvalidate;
// no guest cache tags, executable allocations, or source readiness are modeled.
std::uint64_t NativeInstructionCacheSequence() noexcept;
}
