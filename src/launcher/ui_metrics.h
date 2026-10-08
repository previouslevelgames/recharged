#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

// Launcher scale model, independent of SDL/ImGui so it can be unit tested.
//
// SDL reports two numbers per window:
//  * pixel density: framebuffer pixels per window unit (2.0 on a retina Mac,
//    1.0 on Windows, where window units already are pixels);
//  * display scale: the scale content should be drawn at, in pixels. It is
//    the pixel density times the desktop's content scale (1.5 for Windows at
//    150 %, 1.0 for a macOS default resolution).
// Layout is authored in design units ("dp"). One dp is `ui` window units, and
// glyphs are rasterized at `ui * density` pixels per dp, so text stays sharp
// and the layout keeps the same physical size on every platform.
namespace mscharged::launcher
{
struct DisplayScale
{
    float display_scale = 1.0f;
    float pixel_density = 1.0f;
};

struct UiMetrics
{
    float ui = 1.0f;          // window units per design unit
    float density = 1.0f;     // framebuffer pixels per window unit
    float font_raster = 1.0f; // framebuffer pixels per design unit
};

// Smallest layout the launcher supports, in design units.
inline constexpr float kMinimumDesignWidth = 860.0f;
inline constexpr float kMinimumDesignHeight = 600.0f;
inline constexpr float kDefaultDesignWidth = 1120.0f;
inline constexpr float kDefaultDesignHeight = 720.0f;

inline float SaneScale(float value)
{
    return std::isfinite(value) && value > 0.0f ? value : 1.0f;
}

// user_scale: 0 selects automatic sizing, otherwise a multiplier (1.0 = 100 %).
// usable_width/height: usable display area in window units, or 0 when unknown.
// Automatic sizing shrinks only as far as needed for the minimum layout to fit.
inline UiMetrics ComputeUiMetrics(DisplayScale display, float user_scale,
                                  float usable_width = 0.0f, float usable_height = 0.0f)
{
    UiMetrics metrics;
    metrics.density = std::clamp(SaneScale(display.pixel_density), 1.0f, 4.0f);
    const float content = std::clamp(SaneScale(display.display_scale) / metrics.density, 0.5f, 4.0f);
    if (user_scale > 0.0f && std::isfinite(user_scale))
        metrics.ui = std::clamp(content * user_scale, 0.5f, 6.0f);
    else
    {
        metrics.ui = content;
        if (usable_width > 0.0f && usable_height > 0.0f)
        {
            const float fit = std::min(usable_width / (kMinimumDesignWidth + 40.0f),
                                       usable_height / (kMinimumDesignHeight + 60.0f));
            if (fit < metrics.ui) metrics.ui = std::max(0.75f, fit);
        }
    }
    metrics.font_raster = metrics.ui * metrics.density;
    return metrics;
}

// launcher.ui_scale: "auto" -> 0, "75".."200" -> multiplier, anything else -> -1.
inline float ParseUiScaleSetting(std::string_view value)
{
    if (value == "auto") return 0.0f;
    int percent = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), percent);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || percent < 75 || percent > 200)
        return -1.0f;
    return float(percent) / 100.0f;
}

struct WindowExtent
{
    int width = 0;
    int height = 0;
};

// Window size for a design size, kept inside the usable display area.
inline WindowExtent FitWindow(float design_width, float design_height, float ui,
                              float usable_width, float usable_height)
{
    float width = design_width * ui;
    float height = design_height * ui;
    if (usable_width > 0.0f) width = std::min(width, std::max(1.0f, usable_width - 32.0f * ui));
    if (usable_height > 0.0f) height = std::min(height, std::max(1.0f, usable_height - 48.0f * ui));
    return {int(std::lround(width)), int(std::lround(height))};
}
} // namespace mscharged::launcher
