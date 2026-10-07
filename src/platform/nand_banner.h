#pragma once

#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
// Source SaveLoad produces native Wii32 header fields, Wii16 text, and raw GX
// RGB5A3 pixels. This boundary copies those exact fields to a Wii byte snapshot;
// the source banner and its original allocation stay unchanged.
std::int32_t WriteOriginalNANDBanner(const void* native_banner,
                                   std::uint32_t bytes,
                                   void (*callback)(std::int32_t));

// An unfinished snapshot cannot be retired before its genuine source flash
// callback. This query supports the module/device terminal ownership gate.
bool OriginalNANDBannerWritePending() noexcept;
}
