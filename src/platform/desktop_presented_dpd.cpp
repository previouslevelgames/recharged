#include "platform/desktop_presented_dpd.h"
#include "platform/desktop_dpd_projection.h"

#include <SDL3/SDL.h>

namespace mscharged::platform {
bool QueryPresentedDesktopDpd(void*, SDL_Window* window, DesktopDpdProjection* projection) {
    if (!projection) return false;
    *projection = {};
    if (!window) return false;
    DesktopDpdWindow dimensions{};
    dimensions.id = SDL_GetWindowID(window);
    if (!dimensions.id ||
        !SDL_GetWindowSize(window, &dimensions.logical_width, &dimensions.logical_height) ||
        !SDL_GetWindowSizeInPixels(window, &dimensions.pixel_width, &dimensions.pixel_height))
        return false;
    AuroraVIPresentedGeometry presented{};
    if (!aurora_get_presented_video_geometry(&presented)) return false;
    return ProjectPresentedDesktopDpd(presented, dimensions, projection);
}
} // namespace mscharged::platform
