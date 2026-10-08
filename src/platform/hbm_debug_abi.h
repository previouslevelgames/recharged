#pragma once

#include <stddef.h>
#include <stdint.h>
#if !defined(__cplusplus)
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Address-region compatibility only. This does not validate an object, its
// completed data or its allocation incarnation, and grants no lifetime lease.
// The caller retains the native SDK/thread/image/storage while checking it.
bool ChargedNativeHBMPointerValid(const void* pointer);
// The original DVD map reader uses a Wii32 encoded file cursor, not live RAM.
uint32_t ChargedNativeHBMMapCursorWord(const void* cursor);

typedef struct ChargedNativeHBMFrame {
    uintptr_t stack_address;
    uintptr_t instruction_address;
} ChargedNativeHBMFrame;
// Native unwind metadata replaces the hardware PPC backchain. No instructions
// from the original disc are run and no fabricated chain is published.
size_t ChargedNativeHBMCaptureStack(uintptr_t caller_frame,
    ChargedNativeHBMFrame* frames, size_t capacity);
const char* ChargedNativeHBMStackSymbol(uintptr_t instruction_address);
void ChargedNativeHBMHalt(void);

#ifdef __cplusplus
}
#endif
