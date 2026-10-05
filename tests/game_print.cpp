#include "NL/nlPrint.h"
#include <cstring>
#include <array>

int main()
{
    // Original nlFont::Load keeps the base name in the destination and passes
    // that same pointer as the first %s. Preserve MSL's observable byte result.
    char font[265] = "fe/fonts/eurfonttext18";
    if (nlSNPrintf(font, sizeof(font), "%s_%d", font, 1) != 24
        || std::strcmp(font, "fe/fonts/eurfonttext18_1") != 0) return 2;
    std::strcpy(font, "fe/fonts/eurfontheading36");
    if (nlSNPrintf(font, sizeof(font), "%s_%d", font, 2) != 27
        || std::strcmp(font, "fe/fonts/eurfontheading36_2") != 0) return 3;
    std::array<char, 12> guarded;
    guarded.fill('!');
    std::memcpy(guarded.data()+1, "prefix", 7);
    if (nlSNPrintf(guarded.data()+1, 8, "%s_%d", guarded.data()+1, 123) != 10
        || std::strcmp(guarded.data()+1, "prefix_") != 0
        || guarded[0] != '!' || guarded[9] != '!' || guarded[10] != '!' || guarded[11] != '!') return 4;
    char suffix[32] = "parent/child";
    if (nlSNPrintf(suffix, sizeof(suffix), "%s:%04x", suffix+7, 42) != 10
        || std::strcmp(suffix, "child:002a") != 0) return 5;
    std::array<char, 8> bytes;
    bytes.fill('!');
    if (nlSNPrintf(bytes.data(), bytes.size(), "%cX", 0) != 2
        || bytes[0] != 0 || bytes[1] != 'X' || bytes[2] != 0
        || bytes[3] != '!' || bytes[6] != '!' || bytes[7] != 0) return 6;
    // The source wrapper unconditionally indexes size-1; zero capacity is not
    // a valid wrapper call. The separate host-service qualifier covers count 0.
    // This call must reach the native adapter even when the console header
    // provides an inline no-op for the original game's weak definition.
    constexpr char expected[] = "Native diagnostics: camera 37 0x2a\n";
    return nlPrintf("Native diagnostics: %s %d 0x%02x\n", "camera", 37, 42)
        == static_cast<int>(std::strlen(expected)) ? 0 : 1;
}
