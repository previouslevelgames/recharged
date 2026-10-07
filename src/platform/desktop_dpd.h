#pragma once

#include <array>
#include <cstdint>

namespace mscharged::platform {

// A post-parser WPAD camera observation, before original KPAD processing.
// No game position, validity flag or listener is transported here.
struct NativeDpdObject {
    std::int16_t x{};
    std::int16_t y{767};
    std::uint16_t size{};
    std::uint8_t trace_id{};
};
using NativeDpdObservation = std::array<NativeDpdObject, 4>;

// A live explicit SDL virtual device is the raw observation producer.
// The generation prevents a retired/reused SDL identity from publishing data.
struct NativeDpdSource {
    std::uint32_t joystick_id{};
    std::uint64_t generation{};
};
NativeDpdSource AttachNativeWpadDpdSource(std::uint32_t joystick_id);
void SubmitNativeWpadDpdObservation(NativeDpdSource source,
                                   const NativeDpdObservation& observation);
void DetachNativeWpadDpdSource(NativeDpdSource source);

// Explicit nominal virtual-camera calibration: the source SDK's resolution,
// sensor-height convention, unit-distance pair and upright gravity. Values
// represent real normalized desktop observations; KPAD still selects objects,
// calculates validity and applies its original filtering. Physical IR/EEPROM
// calibration and roll are not claimed by this desktop camera profile.
NativeDpdObservation MakeDesktopDpdObservation(float x, float y,
                                               std::uint8_t sensor_bar_position,
                                               bool visible);

} // namespace mscharged::platform
