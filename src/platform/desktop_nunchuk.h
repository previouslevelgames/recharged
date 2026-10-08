#pragma once

#include <cstdint>

namespace mscharged::platform {

// A post-parser WPAD freestyle (Nunchuk) observation, before original KPAD and
// pad processing: stick counts after the WPAD centre calibration, C and Z, and
// acceleration counts after the zero-g calibration. No game action is sent.
struct NativeNunchukObservation {
    std::int8_t stick_x{}, stick_y{};
    bool c{}, z{};
    std::int16_t acc_x{}, acc_y{}, acc_z{};
};

// Nominal calibration of the virtual Nunchuk: 1 g is 200 counts per axis (the
// source KPAD fallback unit), and a level, untouched Nunchuk reads +1 g on Z.
inline constexpr std::int16_t kNativeNunchukGravity = 200;

// A live explicit SDL virtual device carries one attached virtual Nunchuk.
// While attached, later WPAD services report the original extension sequence
// (INITIALIZING, then FREESTYLE); after detaching they report CORE again.
// The generation prevents a retired/reused SDL identity from publishing data.
struct NativeNunchukSource {
    std::uint32_t joystick_id{};
    std::uint64_t generation{};
};
NativeNunchukSource AttachNativeWpadNunchukSource(std::uint32_t joystick_id);
void SubmitNativeWpadNunchukObservation(NativeNunchukSource source,
                                        const NativeNunchukObservation& observation);
void DetachNativeWpadNunchukSource(NativeNunchukSource source);

} // namespace mscharged::platform
