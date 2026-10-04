#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace mscharged::resources
{
using Bytes = std::span<const std::uint8_t>;
// A structurally readable feature outside the selected native implementation.
// Keep it distinct from malformed data, allocation failures and missing assets.
class UnsupportedResource : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
inline void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
inline Bytes Slice(Bytes data, std::size_t offset, std::size_t size)
{
    Require(offset <= data.size() && size <= data.size() - offset, "Wii asset range exceeds its buffer");
    return data.subspan(offset, size);
}
inline std::uint16_t U16(Bytes data, std::size_t offset)
{
    const auto b = Slice(data, offset, 2);
    return (std::uint16_t(b[0]) << 8) | b[1];
}
inline std::uint32_t U32(Bytes data, std::size_t offset)
{
    const auto b = Slice(data, offset, 4);
    return (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
}
inline float F32(Bytes data, std::size_t offset)
{
    const auto value = std::bit_cast<float>(U32(data, offset));
    Require(std::isfinite(value) && std::abs(value) <= 1e7f, "Nonfinite or excessive Wii model coordinate");
    return value;
}
inline std::size_t Align(std::size_t value, std::size_t alignment)
{
    Require(value <= SIZE_MAX - (alignment - 1), "Wii asset alignment overflow");
    return (value + alignment - 1) & ~(alignment - 1);
}
inline std::size_t Records(Bytes data, std::size_t width, std::size_t maximum)
{
    Require(data.size() % width == 0 && data.size() / width <= maximum, "Invalid Wii asset record count");
    return data.size() / width;
}
inline constexpr std::size_t MaximumAssetBytes = 16 * 1024 * 1024;
}
