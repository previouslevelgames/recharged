#pragma once

#include <cstdint>
#include <limits>

namespace mscharged::platform
{
// runtime.c::__cvt_fp2unsigned first returns zero for values below zero;
// its bge after fcmpu takes both >= 2^32 and unordered inputs to UINT32_MAX.
// Only the remaining finite [0, 2^32) interval reaches the truncating cast.
inline std::uint32_t VariantFloatToWiiWord(float value) noexcept
{
    if (value < 0.0f)
        return 0;
    if (!(value < 4294967296.0f))
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(value);
}
}
