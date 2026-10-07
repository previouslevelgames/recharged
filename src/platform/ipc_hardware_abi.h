#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void* ChargedNativeIPCBufferStart(void);
void* ChargedNativeIPCBufferEnd(void);
uint32_t ChargedNativeIPCReadRegister(int32_t index);
void ChargedNativeIPCWriteRegister(int32_t index, uint32_t value);
#ifdef __cplusplus
}
#endif
