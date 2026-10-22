#include "platform/desktop_presented_dpd.h"
#include "platform/desktop_dpd_projection.h"

#include <SDL3/SDL.h>

#include <atomic>

namespace mscharged::platform {
namespace {
std::atomic<float> sPointerShape{1.0f};
}

void SetPresentedPointerShape(float shape) {
    sPointerShape = shape > 0.0f ? shape : 1.0f;
}

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
    if (!ProjectPresentedDesktopDpd(presented, dimensions, projection)) return false;
    // Wider view: the picture's 16:9 middle part, where the HUD keeps its layout.
    const float shape = sPointerShape.load();
    if (shape > 1.0f) {
        const float width = projection->width / shape;
        projection->left += (projection->width - width) / 2.0f;
        projection->width = width;
    } else if (shape < 1.0f) {
        const float height = projection->height * shape;
        projection->top += (projection->height - height) / 2.0f;
        projection->height = height;
    }
    return true;
}
} // namespace mscharged::platform
