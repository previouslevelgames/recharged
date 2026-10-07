#include "platform/desktop_dpd_projection.h"

#include <cmath>

namespace mscharged::platform {
bool ProjectPresentedDesktopDpd(const AuroraVIPresentedGeometry& presented,
                               const DesktopDpdWindow& window,
                               DesktopDpdProjection* projection) {
    if (!projection) return false;
    *projection = {};
    const auto& completion = presented.completion;
    const auto& scanout = presented.scanout;
    if (!presented.window_incarnation || !window.id || presented.window_id != window.id ||
        window.logical_width <= 0 || window.logical_height <= 0 ||
        window.pixel_width <= 0 || window.pixel_height <= 0 ||
        presented.logical_width != std::uint32_t(window.logical_width) ||
        presented.logical_height != std::uint32_t(window.logical_height) ||
        presented.surface_width != std::uint32_t(window.pixel_width) ||
        presented.surface_height != std::uint32_t(window.pixel_height) ||
        !completion.framebuffer || !completion.copy_revision ||
        !completion.presentation_sequence || completion.black || scanout.black ||
        completion.framebuffer != scanout.framebuffer ||
        completion.retrace_count != scanout.retrace_count ||
        completion.field != scanout.field ||
        !scanout.fb_width || !scanout.xfb_height ||
        !scanout.active_width || !scanout.active_height ||
        !scanout.vi_width || !scanout.vi_height ||
        unsigned(scanout.x_origin) + scanout.vi_width > scanout.active_width ||
        unsigned(scanout.y_origin) + scanout.vi_height > scanout.active_height ||
        scanout.pan_x || scanout.pan_y ||
        scanout.pan_width != scanout.fb_width || scanout.pan_height != scanout.xfb_height)
        return false;
    if (!std::isfinite(presented.viewport_x) || !std::isfinite(presented.viewport_y) ||
        !std::isfinite(presented.viewport_width) || !std::isfinite(presented.viewport_height) ||
        presented.viewport_x < 0 || presented.viewport_y < 0 ||
        presented.viewport_width <= 0 || presented.viewport_height <= 0 ||
        double(presented.viewport_x) + presented.viewport_width > presented.surface_width ||
        double(presented.viewport_y) + presented.viewport_height > presented.surface_height)
        return false;

    // The output shader places the source VI rectangle within its active
    // signal, then within the exact viewport used by Surface::Present.
    // Convert acquired surface pixels to SDL event coordinates only last.
    const double x_scale = double(window.logical_width) / presented.surface_width;
    const double y_scale = double(window.logical_height) / presented.surface_height;
    projection->presented_revision = completion.presentation_sequence;
    projection->left = float((presented.viewport_x +
        double(presented.viewport_width) * scanout.x_origin / scanout.active_width) * x_scale);
    projection->top = float((presented.viewport_y +
        double(presented.viewport_height) * scanout.y_origin / scanout.active_height) * y_scale);
    projection->width = float(double(presented.viewport_width) *
        scanout.vi_width / scanout.active_width * x_scale);
    projection->height = float(double(presented.viewport_height) *
        scanout.vi_height / scanout.active_height * y_scale);
    return true;
}
} // namespace mscharged::platform
