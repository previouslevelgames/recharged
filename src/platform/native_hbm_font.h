#pragma once
#include <cstdint>

namespace mscharged::platform {
// Representation only: the original owner retains the raw resource identity.
// Original ResFont::Rebuild still resolves pointers and writes RFNU itself.
void* NativeHBMFontHeader(void* source);
void* ResolveNativeHBMFontOffset(void* nativeHeader, std::uintptr_t wireOffset);
}
