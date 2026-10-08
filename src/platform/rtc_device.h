#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace mscharged::platform {
// Exact virtual device image, independent of SC records or game save data.
// SRAM retains its64 Wii bytes; flags are the opaque hardware32-bit register.
struct NativeRTCImage {
    std::array<std::uint8_t,64> sram{};
    std::uint32_t flags{};
};
// Existing backing must contain64 SRAM bytes followed by BE32 flags.
// A missing file requires an explicitly supplied initial image. No implicit
// zero/default/locale/disc image or source initialization flag is supplied.
void InitializeNativeRTC(const std::filesystem::path& backing,
                         std::optional<NativeRTCImage> initial);
// Reject a borrowed EXI lock/selection, queued retry or active source callback.
// Source users/cache/module must outlive their borrowed requests.
void ShutdownNativeRTC();
NativeRTCImage ReadNativeRTCImage();
} // namespace mscharged::platform
