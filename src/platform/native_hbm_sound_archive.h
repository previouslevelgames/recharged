#pragma once
#include <cstddef>
#include <cstdint>

namespace mscharged::platform {
// These views retain the real completed raw owner/incarnation. No view owns or
// extends its source lifetime; the original archive/file caller retains it.
const void* NativeHBMSoundArchiveHeader(const void* source);
const void* NativeHBMSoundArchiveInfo(const void* source, std::size_t bytes, std::uint16_t version);
const void* NativeHBMSoundArchiveSymbols(const void* source, std::size_t bytes);
}
