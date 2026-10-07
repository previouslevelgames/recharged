#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Storage descriptions only: actual original AX statics, before AX initialization.
// The host must retain their containing image and reserve physical backing before
// the original game captures the remaining arena. No source state is initialized.
typedef struct ChargedAXStorage {
    void* address;
    uint32_t bytes;
} ChargedAXStorage;
ChargedAXStorage ChargedAXGetCommandStorage(void);
void ChargedAXGetVoiceStorage(ChargedAXStorage output[2]); // AXPB, opaque ITD.
void ChargedAXGetAuxStorage(ChargedAXStorage output[3]);
ChargedAXStorage ChargedAXGetCompressorStorage(void);

// Actual AXOut backing: LR stereo PCM16, surround numeric32, remote PCM16,
// opaque DRAM context and raw firmware, in that order. Task metadata is CPU-only;
// it retains native pointers/callbacks and must not be pinned as a DSP record.
void ChargedAXGetOutputStorage(ChargedAXStorage output[5]);
ChargedAXStorage ChargedAXGetTaskStorage(void);

#ifdef __cplusplus
}
#endif
