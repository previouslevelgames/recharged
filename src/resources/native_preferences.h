#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace mscharged::resources
{
// Typed original UserOptions fields; serialization is a separate native format.
// Audio order is Music, SFX, Voice. No UserInfo/native pointer layout is stored.
struct NativePreferencesValues
{
    using Handle = std::shared_ptr<const NativePreferencesValues>;
    std::array<int,3> audio{}, audio_defaults{};
    bool auto_zoom = false;
    float camera_zoom = 0;
    bool operator==(const NativePreferencesValues&) const = default;
};
using NativePreferencesBytes = std::array<std::uint8_t,64>;
NativePreferencesValues DefaultNativePreferences();
void ValidateNativePreferences(const NativePreferencesValues&);
NativePreferencesBytes EncodeNativePreferences(const NativePreferencesValues&);
NativePreferencesValues DecodeNativePreferences(std::span<const std::uint8_t>);
}
