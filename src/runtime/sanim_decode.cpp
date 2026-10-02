// Scalar equivalents of Charged's fixed GQR6/GQR7 quantized loads.
#include "Game/SAnimDecode.h"

#include <limits>

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559
              && std::numeric_limits<float>::digits == 24,
              "SAnim decoding requires IEEE 754 binary32 floats.");

namespace
{
float DecodeSigned(unsigned value, unsigned sign, float scale)
{
    // Avoid implementation-defined unsigned-to-signed narrowing.
    const int integer = value & sign ? static_cast<int>(value) - static_cast<int>(2 * sign)
                                    : static_cast<int>(value);
    return static_cast<float>(integer) * scale;
}
}

void SAnimInitGQR()
{
    // The console only sets GQR6=0x0f070f07 and GQR7=0x07060706 here.
    // Native decoders encode those signed formats/scales directly, so there
    // is no register state to initialize or reset. All decoder bodies are real.
}

extern "C" void SAnimDecodeRot16(nlQuaternion* result, const void* packed)
{
    const auto* bytes = static_cast<const unsigned char*>(packed);
    for (unsigned i = 0; i < 4; ++i)
    {
        const unsigned value = (static_cast<unsigned>(bytes[2 * i]) << 8) | bytes[2 * i + 1];
        result->e[i] = DecodeSigned(value, 0x8000, 1.0f / 32768.0f);
    }
}

extern "C" void SAnimDecodeRot12(nlQuaternion* result, const void* packed)
{
    const auto* bytes = static_cast<const unsigned char*>(packed);
    for (unsigned pair = 0; pair < 2; ++pair)
    {
        const auto* key = bytes + 3 * pair;
        const unsigned first = (static_cast<unsigned>(key[0]) << 4) | (key[1] >> 4);
        const unsigned second = (static_cast<unsigned>(key[1] & 0x0f) << 8) | key[2];
        // Original unpacking shifts each signed 12-bit component left four
        // bits, then uses GQR6's signed 16-bit scale of 2^-15.
        result->e[2 * pair] = DecodeSigned(first, 0x800, 1.0f / 2048.0f);
        result->e[2 * pair + 1] = DecodeSigned(second, 0x800, 1.0f / 2048.0f);
    }
}

extern "C" void SAnimDecodeRot8(nlQuaternion* result, const void* packed)
{
    const auto* bytes = static_cast<const unsigned char*>(packed);
    for (unsigned i = 0; i < 4; ++i)
        result->e[i] = DecodeSigned(bytes[i], 0x80, 1.0f / 128.0f);
}
