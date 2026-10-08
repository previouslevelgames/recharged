#pragma once
#include <aurora/aurora.h>

namespace mscharged::platform
{
#if defined(__APPLE__)
inline constexpr AuroraBackend NativeGraphicsBackend = BACKEND_METAL;
inline constexpr const char* NativeGraphicsBackendName = "Metal";
#else
inline constexpr AuroraBackend NativeGraphicsBackend = BACKEND_VULKAN;
inline constexpr const char* NativeGraphicsBackendName = "Vulkan";
#endif
}
