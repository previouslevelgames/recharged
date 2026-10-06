#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Exact 16-bit hardware access; original expressions decide masks and order.
// Unsupported reset/bootstrap requests fail before source readiness can change.
uint16_t ChargedDSPControlRead(void);
void ChargedDSPControlWrite(uint16_t value);
#ifdef __cplusplus
}
#endif
