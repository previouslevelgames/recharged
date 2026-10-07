#include "dsp_boot_data.h"
// Whole prepared OS/audio source and original AX firmware data.
// No SDK runtime link, source extraction or section collection.
#include "src/RVL_SDK/os/OSAudioSystem.c"
#include "src/RVL_SDK/ax/DSPCode.c"

OriginalDSPBootData OriginalDSPBootDataForFixture(void) {
    OriginalDSPBootData result={DSPInitCode,sizeof(DSPInitCode)};return result;
}
OriginalDSPBootData OriginalAXFirmwareForOSBootFixture(void) {
    OriginalDSPBootData result={axDspSlave,sizeof(axDspSlave)};return result;
}
