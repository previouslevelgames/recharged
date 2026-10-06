#include "dsp_boot_data.h"
#include <stdarg.h>
#include <dolphin/os.h>

// Fixture-only declaration compatibility for the discarded hardware sections:
// canonical native SDK owns OSThreadQueue/OSContext; the original OS.h declares
// this flag. No provider, value or successful hardware service is supplied.
#define REVOLUTION_OS_THREAD_H
extern BOOL __OSInIPL;
// Literal original OSTime.h spelling needed while its discarded wait helper
// is compiled against the canonical native clock declarations.
#define OS_TICKS_DELTA(x, y) ((s32)(x) - (s32)(y))

// Actual complete original TU in this explicitly data-only leaf. Unrelated
// MMIO/service functions are section-collected; none is substituted/executed.
#include "src/RVL_SDK/os/OSAudioSystem.c"

__attribute__((visibility("default")))
OriginalDSPBootData OriginalDSPBootDataForFixture(void) {
    OriginalDSPBootData result = {DSPInitCode, sizeof(DSPInitCode)};
    return result;
}
