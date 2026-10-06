// Byte/word ABI qualification beneath the complete original lighting methods.
// This fixture executes no game constructor, loading lifecycle or GPU command.
#include "platform/gx_data_transport.h"
#include "Game/Render/LightingLookup.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>

#if defined(MSCHARGED_DIAGNOSTIC_LIGHTING)
#error The original lighting ABI gate must use the original class declaration.
#endif

namespace {
unsigned checks;
void Check(bool value)
{
    ++checks;
    if (!value) throw std::runtime_error("Original lighting data ABI mismatch");
}
template <class Word>
auto WiiBytes(Word word)
{
    auto bytes = std::bit_cast<std::array<unsigned char, sizeof(Word)>>(word);
    if constexpr (std::endian::native == std::endian::little)
        std::reverse(bytes.begin(), bytes.end());
    return bytes;
}
void CheckColour(u32 word)
{
    const auto bytes = WiiBytes(word);
    const nlColour source{{bytes[0], bytes[1], bytes[2], bytes[3]}};
    Check(mscharged::platform::ColourToWiiWord(source) == word);
    const GXColor native = mscharged::platform::ColourFromWiiWord(word);
    Check(native.r == bytes[0]); Check(native.g == bytes[1]);
    Check(native.b == bytes[2]); Check(native.a == bytes[3]);
}
} // namespace

int main()
{
    static_assert(sizeof(u16) == 2 && sizeof(u32) == 4);
    static_assert(sizeof(nlColour) == 4 && sizeof(GXColor) == 4);
    static_assert(sizeof(LightingLookup) == sizeof(void*) + 2 * sizeof(int));
    try
    {
        for (unsigned value = 0; value < 65536; ++value)
        {
            const auto bytes = WiiBytes(u16(value));
            alignas(2) unsigned char raw[4] = {0x5a, bytes[0], bytes[1], 0xa5};
            Check(mscharged::platform::ReadWiiPaletteWord(
                reinterpret_cast<const u16*>(raw + 1)) == value);
            Check(raw[0] == 0x5a && raw[3] == 0xa5);
        }
        for (unsigned channel = 0; channel < 4; ++channel)
            for (unsigned value = 0; value < 256; ++value)
                CheckColour(0x09ff7b01u ^ (u32(value) << (8 * channel)));
        for (u32 word : {0u, 1u, 0x000000ffu, 0xffffffffu, 0x80000000u,
            0x12345678u, 0x7f3a6101u, 0x01020304u})
            CheckColour(word);
        std::printf("original lighting byte transport checks=%u source_ctors=unexecuted load_lifecycle=HOLD GPU=unexecuted\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
