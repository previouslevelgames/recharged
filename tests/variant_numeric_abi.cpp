#include "Game/AI/Variant.inl"
#include "platform/variant_numeric_abi.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

static unsigned checks;

static void Check(bool result, const char* message)
{
    if (!result)
        throw std::runtime_error(message);
    ++checks;
}

static float Float(std::uint32_t bits)
{
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

// Independent IEEE32 word oracle: no float-to-integer cast. runtime.c returns
// all ones for unordered, zero for negatives, then truncates/saturates positives.
static std::uint32_t WiiFloatWord(std::uint32_t bits)
{
    const std::uint32_t magnitude = bits & 0x7fffffffU;
    if (magnitude > 0x7f800000U)
        return 0xffffffffU;
    if (bits >> 31)
        return 0;
    const unsigned exponent = magnitude >> 23;
    if (exponent < 127)
        return 0;
    if (exponent >= 159)
        return 0xffffffffU;
    const std::uint64_t significand = (magnitude & 0x7fffffU) | 0x800000U;
    const unsigned shift = exponent - 127;
    return static_cast<std::uint32_t>(shift >= 23
        ? significand << (shift - 23) : significand >> (23 - shift));
}

int main()
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    using Data = decltype(Variant::mData);
    static_assert(sizeof(Data::u) == 4);
    static_assert(sizeof(Data::pointer) == sizeof(void*));
    try
    {
        for (unsigned exponent = 0; exponent != 256; ++exponent)
        {
            for (std::uint32_t mantissa : {0U, 1U, 0x400000U, 0x7fffffU})
            {
                for (std::uint32_t sign : {0U, 0x80000000U})
                {
                    const auto bits = sign | (exponent << 23) | mantissa;
                    Check(mscharged::platform::VariantFloatToWiiWord(Float(bits)) == WiiFloatWord(bits),
                        "native float intrinsic differs from independent IEEE word oracle");
                }
            }
        }
        const std::uint32_t values[] = {0, 1, 0x7fffffffU, 0x80000000U, 0xffffffffU};
        for (auto value : values)
        {
            Data data;
            std::memset(&data, 0xa5, sizeof(data));
            std::memcpy(&data, &value, sizeof(value));
            Check(data.u == value, "actual original numeric member reads native padding");
        }
        alignas(16) std::array<unsigned char, 16> backing{};
        for (void* pointer : std::array<void*, 3>{nullptr, backing.data(), backing.data() + 8})
        {
            Data data{};
            data.pointer = pointer;
            Check(reinterpret_cast<std::uintptr_t>(data.pointer) == reinterpret_cast<std::uintptr_t>(pointer),
                "actual pointer member truncates its real backing");
            std::uintptr_t bytes;
            std::memcpy(&bytes, &data, sizeof(bytes));
            Check(bytes == reinterpret_cast<std::uintptr_t>(pointer), "pointer alias representation differs");
        }
        if constexpr (sizeof(void*) > 4)
            Check(reinterpret_cast<std::uintptr_t>(backing.data()) > 0xffffffffULL,
                "this host did not exercise actual high pointer bits");
        std::printf("PASS %u intrinsic/actual-union checks; real pointer %p. No whole class/cache/actor execution claim.\n",
            checks, static_cast<void*>(backing.data()));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s after %u checks\n", e.what(), checks);
        return 1;
    }
}
