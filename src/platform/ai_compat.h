#pragma once

#include <dolphin/ai.h>

typedef AIDCallback AIDMACallback;
typedef enum {
    AI_DSP_32KHZ = AI_SAMPLERATE_32KHZ,
    AI_DSP_48KHZ = AI_SAMPLERATE_48KHZ,
} AIDSPSampleRate;

// Original RVL callers pass a buffer pointer; the native canonical interface
// carries its complete address. Macros also support untouched original C TUs.
#define AIInitDMA(buffer, length) AIInitDMA((uintptr_t)(buffer), (length))
#define AIInit(stack) AIInit((u8*)(stack))
