#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Exact register accesses underneath original OSAudioSystem. Unknown registers,
// missing native owner/banks and unsupported requests fail; no ready state.
uint16_t ChargedOSAudioDSPRead(uint32_t reg);
void ChargedOSAudioDSPWrite(uint32_t reg,uint16_t value);
uint32_t ChargedOSAudioDSPReadPair(uint32_t high_reg);
void ChargedOSAudioDSPWritePair(uint32_t high_reg,uint32_t value);
uint32_t ChargedOSAudioIPCRead(uint32_t reg);
void ChargedOSAudioIPCWrite(uint32_t reg,uint32_t value);
void* ChargedOSAudioWorkMemory(void);
#ifdef __cplusplus
}
#endif
