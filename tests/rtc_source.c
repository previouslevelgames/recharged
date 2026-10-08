// Whole original TU; no source cache/flag seed or SDK runtime in this object.
#include "src/RVL_SDK/os/OSRtc.c"
BOOL ChargedRTCCommitBaseForFixture(void) {
    // Paired original private lock/commit API, using only bytes loaded by the
    // preceding real source __OSInitSram/EXI read. No field is written here.
    if (!LockSram(0)) return FALSE;
    return UnlockSram(TRUE, 0);
}
