#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

namespace mscharged::platform
{
// Native lowering of a two-lane signed psq_l with a fixed GQR scale, followed
// by the unquantized psq_st format. Encoded input bytes remain big endian;
// stores write host-order IEEE floats and require no alignment.
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559
    && std::numeric_limits<float>::digits == 24
    && std::numeric_limits<unsigned char>::digits == 8
    && std::numeric_limits<int>::digits >= 31);

struct QuantizedPair
{
    float first;
    float second;
};

template<unsigned Bits, unsigned FractionBits>
inline QuantizedPair LoadSignedQuantizedPair(const void* encoded)
{
    static_assert(Bits == 8 || Bits == 16);
    static_assert(FractionBits < 64);
    constexpr float scale = 1.0f / static_cast<float>(std::uint64_t{1} << FractionBits);
    constexpr unsigned sign = 1u << (Bits - 1);
    const auto* bytes = static_cast<const unsigned char*>(encoded);
    auto lane = [&](unsigned index) {
        unsigned word;
        if constexpr (Bits == 16)
            word = (unsigned(bytes[2 * index]) << 8) | bytes[2 * index + 1];
        else
            word = bytes[index];
        const int integer = word & sign ? int(word) - int(2 * sign) : int(word);
        return static_cast<float>(integer) * scale;
    };
    return {lane(0), lane(1)};
}

inline void StoreFloatPair(void* destination, QuantizedPair pair)
{
    auto* bytes = static_cast<unsigned char*>(destination);
    std::memcpy(bytes, &pair.first, sizeof(float));
    std::memcpy(bytes + sizeof(float), &pair.second, sizeof(float));
}
}
