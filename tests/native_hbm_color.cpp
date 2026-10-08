#include "revolution/hbm/nw4hbm/lyt/common.h"
#include "revolution/hbm/nw4hbm/ut/Color.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using nw4hbm::ut::Color;
namespace detail = nw4hbm::lyt::detail;
unsigned checks;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void Bytes(const Color& color, std::array<unsigned char, 4> expected) {
    static_assert(sizeof(Color) == 4 && sizeof(GXColor) == 4);
    Check(std::memcmp(static_cast<const GXColor*>(&color), expected.data(), 4) == 0,
          "Original RGBA scalar lost the four authored channel bytes");
}
}

int main() {
    try {
        Color numeric(0x11223344u);
        Bytes(numeric, {0x11, 0x22, 0x33, 0x44});
        Check(numeric.ToU32() == 0x11223344u && u32(numeric) == 0x11223344u,
              "Original numeric color roundtrip changed");
        Bytes(Color(Color::RED), {255, 0, 0, 255});
        Bytes(Color(Color::GREEN), {0, 255, 0, 255});
        Bytes(Color(Color::BLUE), {0, 0, 255, 255});
        Bytes(Color(Color::BLACK), {0, 0, 0, 255});
        Bytes(Color(Color::GRAY), {128, 128, 128, 255});
        Bytes(Color(), {255, 255, 255, 255});

        const GXColor gx{0x17, 0x39, 0x5b, 0x7d};
        Color copied(gx);
        Bytes(copied, {0x17, 0x39, 0x5b, 0x7d});
        Check(copied.ToU32() == 0x17395b7du, "GX byte copy changed the logical color word");
        numeric = gx;
        Bytes(numeric, {0x17, 0x39, 0x5b, 0x7d});
        numeric = static_cast<const GXColor&>(numeric);
        Bytes(numeric, {0x17, 0x39, 0x5b, 0x7d});
        numeric = Color(0x89abcdefu);
        Bytes(numeric, {0x89, 0xab, 0xcd, 0xef});
        Bytes(numeric & 0xff00ff00u, {0x89, 0, 0xcd, 0});
        Bytes(numeric | 0x00ff0000u, {0x89, 255, 0xcd, 0xef});
        numeric.ToU32ref() = 0x02468aceu;
        Bytes(numeric, {2, 0x46, 0x8a, 0xce});
        copied.ToU32ref() = numeric.ToU32ref();
        Bytes(copied, {2, 0x46, 0x8a, 0xce});
        copied.ToU32ref() = copied.ToU32ref();
        Check(u32(copied.ToU32ref()) == 0x02468aceu &&
                  static_cast<const Color&>(copied).ToU32ref() == 0x02468aceu,
              "Original color word alias lost read/write semantics");
        copied.Set(0x21, 0x43, 0x65, 0x87);
        Check(copied.ToU32() == 0x21436587u, "Channel writes lost canonical RGBA order");

        // Execute the whole original layout color/alpha routines. Distinct
        // channels and partial alpha expose reversals hidden by white colors.
        const Color source[4] = {Color(0x10203040u), Color(0x517293ffu),
                                 Color(0xa2b3c480u), Color(0xd5e6f700u)};
        Color result[4];
        detail::MultipleAlpha(result, source, 128);
        Bytes(result[0], {0x10, 0x20, 0x30, 0x20});
        Bytes(result[1], {0x51, 0x72, 0x93, 0x80});
        Bytes(result[2], {0xa2, 0xb3, 0xc4, 0x40});
        Bytes(result[3], {0xd5, 0xe6, 0xf7, 0});
        Bytes(detail::MultipleAlpha(source[0], 255), {0x10, 0x20, 0x30, 0x40});
        Check(detail::GetVtxColorElement(result, 0) == 0x10 &&
                  detail::GetVtxColorElement(result, 7) == 0x80 &&
                  detail::GetVtxColorElement(result, 10) == 0xc4,
              "Original layout channel indexing changed");
        detail::SetVtxColorElement(result, 5, 0x35);
        Check(result[1].ToU32() == 0x51359380u, "Original animation channel update changed the wrong byte");
        Color white[4];
        Check(!detail::IsModulateVertexColor(white, 255) &&
                  detail::IsModulateVertexColor(white, 127) &&
                  detail::IsModulateVertexColor(result, 255),
              "Original vertex modulation decision changed");
        std::printf("Original HBM color ABI: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "HBM color check %u: %s\n", checks, error.what());
        return 1;
    }
}
