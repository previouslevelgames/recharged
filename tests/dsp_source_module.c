// Bounded data-owner fixture only. Compile the entire original AXOut TU;
// export its exact private static data, without calling its initialization or
// substituting any absent DSP/audio dependency. Unrelated functions are GC'd.
#include "dsp_source_module.h"
#include MSCHARGED_ORIGINAL_AXOUT_FILE
__attribute__((visibility("default")))
OriginalDSPData OriginalDSPDataForFixture(void) {
    OriginalDSPData data = {axDspSlave, axDspSlaveLength, __AXDramImage,
        sizeof(__AXDramImage), axDspInitVector, axDspResumeVector};
    return data;
}
