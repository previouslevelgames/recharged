#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mscharged
{
enum class GraphicsBackend { Vulkan, Direct3D12, Metal };

struct GraphicsBackendChoice
{
    const char* value;
    const char* label;
    GraphicsBackend backend;
};

#if defined(_WIN32)
inline constexpr std::array kGraphicsBackends = {
    GraphicsBackendChoice{"vulkan", "Vulkan", GraphicsBackend::Vulkan},
    GraphicsBackendChoice{"d3d12", "DirectX 12", GraphicsBackend::Direct3D12},
};
#elif defined(__APPLE__)
inline constexpr std::array kGraphicsBackends = {
    GraphicsBackendChoice{"metal", "Metal", GraphicsBackend::Metal},
};
#else
inline constexpr std::array kGraphicsBackends = {
    GraphicsBackendChoice{"vulkan", "Vulkan", GraphicsBackend::Vulkan},
};
#endif

// Keep existing auto settings on the platform's previous default. An explicit
// selection is never silently replaced, including settings from another OS.
inline const GraphicsBackendChoice& ResolveGraphicsBackend(std::string_view setting)
{
    if (setting == "auto") return kGraphicsBackends.front();
    for (const auto& choice : kGraphicsBackends)
        if (setting == choice.value) return choice;
    throw std::invalid_argument("Graphics backend '" + std::string(setting)
        + "' is unavailable on this platform. Set display.backend = auto in the INI to use this platform's default.");
}
}
