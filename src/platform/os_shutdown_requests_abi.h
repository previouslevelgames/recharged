#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t OSGetAppType(void);
void __OSReboot(uint32_t resetCode, uint32_t bootDol);
void __OSLaunchMenu(void);
void __OSRelaunchTitle(void);
void __VISetRGBModeImm(void);
int32_t __PADDisableRecalibration(int32_t disable);

#ifdef __cplusplus
}
#endif
