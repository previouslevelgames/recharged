#pragma once

#include <cstddef>
#include <cstdint>

namespace mscharged {

// Explicit domains: header consumes raw Wii words. Table consumes raw Wii
// entries/pool after the original callback has accepted the native header.
// Both operate in the existing, aligned NL allocation; neither owns storage.
void PrepareWiiLocalizationHeader(void* buffer, std::size_t size);
void PrepareWiiLocalizationTable(void* buffer, std::size_t size,
                                std::uint32_t string_count);

} // namespace mscharged
