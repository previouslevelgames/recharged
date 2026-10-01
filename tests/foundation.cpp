#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>

#include "Game/Core/mtRandom.h"
#include "NL/nlEndian.h"
#include "NL/nlMath.h"

static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::is_same_v<decltype(&seedMT), void (*)(u32)>);
static_assert(sizeof(nlVector3) == 12 && sizeof(nlMatrix4) == 64);

int main()
{
    int failures = 0;
    auto check = [&failures](bool passed, const char* message) {
        if (!passed)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };

    unsigned short swapped = 0;
    nlSwapEndian(0x1234, &swapped);
    check(swapped == 0x3412, "16-bit endian conversion");
    for (unsigned int value = 0; value <= 0xffff; ++value)
    {
        unsigned short roundTrip = 0;
        nlSwapEndian(static_cast<unsigned short>(value), &swapped);
        nlSwapEndian(swapped, &roundTrip);
        if (roundTrip != value)
        {
            check(false, "16-bit endian round trip");
            break;
        }
    }

    unsigned int seed = 0x12345678;
    constexpr unsigned int expected[] = {896, 561, 93, 678, 445, 529, 696, 566};
    for (unsigned int value : expected)
        check(nlRandom(1000, &seed) == value, "Deterministic random sequence");
    check(seed == 0xf53b3b01, "Random state after eight draws");
    const unsigned int previous = seed;
    check(nlRandom(0, &seed) == 0 && seed == previous, "Zero range preserves random state");
    check(nlRandom(1, &seed) == 0, "Unit range");
    for (int i = 0; i < 1000; ++i)
    {
        const float value = nlRandomf(-2.0f, 3.0f, &seed);
        if (!std::isfinite(value) || value < -2.0f || value > 3.0f)
        {
            check(false, "Floating random value within requested bounds");
            break;
        }
    }

    check(nlAbs(-3.25f) == 3.25f, "Absolute value");
    check(!std::signbit(nlAbs(-0.0f)), "Absolute value clears negative zero");
    check(std::isinf(nlAbs(-std::numeric_limits<float>::infinity())), "Absolute infinity");
    check(std::isnan(nlAbs(std::numeric_limits<float>::quiet_NaN())), "Absolute NaN");
    // This snapshot exposes seeding only, so no complete MT sequence is claimed.
    seedMT(0xffffffffU);
    nlInitRandom();
    return failures == 0 ? 0 : 1;
}
