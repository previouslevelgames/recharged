#pragma once
#include "bootstrap/graphics_backend.h"
#include <aurora/aurora.h>

namespace mscharged::platform
{
inline AuroraBackend AuroraGraphicsBackend(GraphicsBackend backend)
{
    switch (backend)
    {
    case GraphicsBackend::Vulkan: return BACKEND_VULKAN;
    case GraphicsBackend::Direct3D12: return BACKEND_D3D12;
    case GraphicsBackend::Metal: return BACKEND_METAL;
    }
    throw std::invalid_argument("Unknown graphics backend");
}

#if defined(__APPLE__)
inline constexpr AuroraBackend NativeGraphicsBackend = BACKEND_METAL;
inline constexpr const char* NativeGraphicsBackendName = "Metal";
#else
inline constexpr AuroraBackend NativeGraphicsBackend = BACKEND_VULKAN;
inline constexpr const char* NativeGraphicsBackendName = "Vulkan";
#endif
}
