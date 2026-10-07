#pragma once

#include "platform/desktop_wpad.h"
#include <aurora/video.h>
#include <cstdint>

namespace mscharged::platform {
// These are actual current SDL dimensions, separate from the dimensions of
// the last successful presentation. A resize must not reuse old calibration.
struct DesktopDpdWindow {
    std::uint32_t id{};
    int logical_width{}, logical_height{};
    int pixel_width{}, pixel_height{};
};

// Pure hardware geometry transport. Full-source VI pan is the qualified
// desktop camera domain; a cropped source pan remains explicitly unavailable.
bool ProjectPresentedDesktopDpd(const AuroraVIPresentedGeometry& presented,
                               const DesktopDpdWindow& window,
                               DesktopDpdProjection* projection);
} // namespace mscharged::platform
