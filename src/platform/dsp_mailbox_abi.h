#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint16_t ChargedDSPMailToHigh(void);
uint16_t ChargedDSPMailFromHigh(void);
uint16_t ChargedDSPMailFromLow(void);
void ChargedDSPMailToWriteHigh(uint16_t value);
void ChargedDSPMailToWriteLow(uint16_t value);
uint32_t ChargedDSPRequireMailWord(uintptr_t value);
#ifdef __cplusplus
}
#endif
#ifdef MSCHARGED_NATIVE
// Canonical native identity for the original Wii source's DSP IRQ selection.
#include <dolphin/os/OSInterrupt.h>
#ifndef OS_INTR_DSP_DSP
#define OS_INTR_DSP_DSP __OS_INTERRUPT_DSP_DSP
#endif
#ifndef OS_INTR_MASK
#define OS_INTR_MASK(interrupt) OS_INTERRUPTMASK(interrupt)
#endif
#endif
