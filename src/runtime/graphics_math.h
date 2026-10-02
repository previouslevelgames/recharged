#pragma once
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace mscharged
{
// Host seed for the original Newton refinements used by the graphics preview.
// This computes a real reciprocal square root. It does not reproduce the Wii
// frsqrte estimate's bit pattern; console floating-point parity remains pending.
inline double NativeReciprocalSqrtEstimate(double value)
{
    return 1.0 / std::sqrt(value);
}

// Preserve fixed-angle truncation and modulo 65536 without an out-of-range
// float-to-short conversion on the host. Wii arithmetic parity is a later gate.
inline std::uint16_t NativeFixedAngle16(float radians)
{
    const float scaled = 10430.378f * radians;
    if (!std::isfinite(scaled))
        throw std::invalid_argument("Fixed-angle conversion requires a finite angle");
    return static_cast<std::uint16_t>(static_cast<std::int32_t>(
        std::fmod(static_cast<double>(scaled), 65536.0)));
}
}
