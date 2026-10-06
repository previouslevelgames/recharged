#pragma once
#include <stdbool.h>
#include <dolphin/os/OSError.h>

// Original RVL_SDK OSError.h assertion spelling, with the real native panic.
#ifndef OSAssert
#define OSAssert(file, line, expression, ...) \
    if (!(expression))                        \
    {                                         \
        OSPanic(file, line, __VA_ARGS__);       \
    }
#endif
#ifdef __cplusplus
extern "C" {
#endif
bool mscharged_stm_reset_button_pressed(void);
void mscharged_stm_disable_video_output(void);
#ifdef __cplusplus
}
#endif
