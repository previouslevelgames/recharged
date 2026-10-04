#include "NL/nlPrint.h"
#include <cstring>

int main()
{
    // This call must reach the native adapter even when the console header
    // provides an inline no-op for the original game's weak definition.
    constexpr char expected[] = "Native diagnostics: camera 37 0x2a\n";
    return nlPrintf("Native diagnostics: %s %d 0x%02x\n", "camera", 37, 42)
        == static_cast<int>(std::strlen(expected)) ? 0 : 1;
}
