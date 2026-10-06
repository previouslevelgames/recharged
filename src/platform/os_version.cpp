#include <dolphin/os.h>

// Native reporting contract of RVL_SDK/os/OS.c::OSRegisterVersion.
// The reconstructed SDK callers keep their original version strings and order.
extern "C" void OSRegisterVersion(const char* version) {
    OSReport("%s\n", version);
}
