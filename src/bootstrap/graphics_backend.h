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

// auto only: the platform's next API, tried when the default cannot start (on
// Windows, DirectX 12 when the driver offers no usable Vulkan GPU).
inline const GraphicsBackendChoice* GraphicsBackendFallback(std::string_view setting)
{
    if (setting != "auto" || kGraphicsBackends.size() < 2) return nullptr;
    return kGraphicsBackends.data() + 1;
}

// What the player reads when no graphics API could start.
inline std::string GraphicsStartFailure(const GraphicsBackendChoice& chosen, const GraphicsBackendChoice* fallback)
{
    if (fallback)
        return std::string("Neither ") + chosen.label + " nor " + fallback->label
            + " could start on this PC. Updating the graphics driver may help.";
    const std::string failed = std::string(chosen.label) + " could not start on this PC.";
    if (kGraphicsBackends.size() > 1)
        return failed + " Choose another graphics API on the launcher's Advanced page, or update the graphics driver.";
    return failed + " Updating the graphics driver may help.";
}
}
