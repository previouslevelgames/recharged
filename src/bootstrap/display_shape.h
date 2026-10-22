#pragma once

// display.aspect and display.resolution (issue #91): the game's layout, the
// picture's shape in its window and the steps of the 3D resolution hotkeys.
//
// The game draws a widescreen (16:9) or a 4:3 layout, chosen once at start
// from the Wii system setting it reads. Its picture keeps that shape in the
// window, with bars beside or above and below it when the window has another
// shape, or with display.aspect = stretch fills the whole window.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace mscharged
{
// The Wii picture's height: the 3D resolution Native, and the unit of 2x-4x.
inline constexpr int kNativeRenderHeight = 448;
inline constexpr int kNativeRenderWidth = 640;

// Whether the game uses its widescreen layout, for display.aspect and the
// shape its picture fills (the display in fullscreen, the window otherwise):
// auto and stretch take the closer of 16:9 and 4:3 to that shape.
inline bool WidescreenLayout(std::string_view aspect, int width, int height)
{
    if (aspect == "16:9") return true;
    if (aspect == "4:3") return false;
    if (width <= 0 || height <= 0) return true;
    // Closer to 16:9 above the geometric mean of the two shapes:
    // (width / height)^2 > 4/3 * 16/9 = 64/27, about 1.54 : 1.
    return double(width) * width * 27.0 > double(height) * height * 64.0;
}

struct ShapeSize
{
    int width = 0, height = 0;
};

// display.fullscreen_resolution: the display mode fullscreen switches the
// display to while the game runs ("1280x720"), or "desktop", which keeps the
// display's own resolution (no mode change).
inline constexpr std::string_view kDesktopResolution = "desktop";
// The size a fullscreen resolution names; {} for desktop or anything else.
inline ShapeSize ParseResolution(std::string_view text)
{
    const auto x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 == text.size()) return {};
    int size[2] = {0, 0};
    for (int part = 0; part < 2; ++part)
        for (const char c : part == 0 ? text.substr(0, x) : text.substr(x + 1))
        {
            if (c < '0' || c > '9' || size[part] > 16384) return {};
            size[part] = size[part] * 10 + (c - '0');
        }
    if (size[0] < 320 || size[0] > 16384 || size[1] < 240 || size[1] > 16384) return {};
    return {size[0], size[1]};
}
inline std::string FormatResolution(int width, int height)
{
    return std::to_string(width) + "x" + std::to_string(height);
}

// A window of `width` that shows an aspect_width:aspect_height picture
// without bars: the same width, and the height of that shape.
inline ShapeSize SnapToShape(int width, int height, int aspect_width, int aspect_height)
{
    if (width <= 0 || height <= 0 || aspect_width <= 0 || aspect_height <= 0) return {width, height};
    return {width, std::max(1, int(std::lround(double(width) * aspect_height / aspect_width)))};
}

// The bars around an aspect_width:aspect_height picture that keeps its shape
// on a width x height screen: their size in pixels on each side, beside the
// picture (sides) or above and below it. 0 when the shapes match.
struct PictureBars
{
    bool sides = false;
    int size = 0;
};
inline PictureBars BarsAround(int width, int height, int aspect_width, int aspect_height)
{
    if (width <= 0 || height <= 0 || aspect_width <= 0 || aspect_height <= 0) return {};
    const double picture = double(aspect_width) / aspect_height;
    if (double(width) / height > picture)
        return {true, int(std::lround((width - height * picture) / 2.0))};
    return {false, int(std::lround((height - width / picture) / 2.0))};
}

// The picture's own size on a width x height screen: without the bars around
// it, or the whole screen when stretched.
inline ShapeSize PictureSize(int width, int height, int aspect_width, int aspect_height, bool stretch)
{
    if (stretch) return {width, height};
    const auto bars = BarsAround(width, height, aspect_width, aspect_height);
    return {width - (bars.sides ? 2 * bars.size : 0), height - (bars.sides ? 0 : 2 * bars.size)};
}

// How much a stretched aspect_width:aspect_height picture changes its shape
// on a width x height screen, in percent of its width: positive when it gets
// wider, negative when narrower.
inline int StretchPercent(int width, int height, int aspect_width, int aspect_height)
{
    if (width <= 0 || height <= 0 || aspect_width <= 0 || aspect_height <= 0) return 0;
    return int(std::lround((double(width) / height / (double(aspect_width) / aspect_height) - 1.0) * 100.0));
}

// display.resolution values, lowest first: the scale the game's EFB is drawn
// at (0 for window, which follows the window's picture).
inline constexpr std::array<std::string_view, 5> kRenderResolutions{"native", "2x", "3x", "4x", "window"};
inline float RenderScale(std::string_view resolution)
{
    if (resolution == "window") return 0.0f;
    if (resolution == "2x") return 2.0f;
    if (resolution == "3x") return 3.0f;
    if (resolution == "4x") return 4.0f;
    return 1.0f;
}

// How players read a display.resolution value: Window, Native, 2x...
inline std::string_view RenderResolutionLabel(std::string_view resolution)
{
    // window: the full resolution of the screen it draws on (the window, or
    // the display in fullscreen).
    if (resolution == "window") return "Full";
    if (resolution == "native") return "Native";
    return resolution;
}

// The width the 3D resolution draws at, for a window picture `picture_width`
// pixels wide (window: that width, at least Native's).
inline int RenderWidth(std::string_view resolution, int picture_width)
{
    const float scale = RenderScale(resolution);
    if (scale == 0.0f) return std::max(kNativeRenderWidth, picture_width);
    return int(std::lround(kNativeRenderWidth * scale));
}

// The height the 3D resolution draws at, when the window's picture is
// `picture_height` pixels high (window: that height, at least Native's).
inline int RenderHeight(std::string_view resolution, int picture_height)
{
    const float scale = RenderScale(resolution);
    if (scale == 0.0f) return std::max(kNativeRenderHeight, picture_height);
    return int(std::lround(kNativeRenderHeight * scale));
}

// The 3D resolution `steps` steps above (positive) or below `current`: each
// step takes the next higher (lower) height on this window; the highest and
// the lowest stay.
inline std::string_view StepRenderResolution(std::string_view current, int steps, int picture_height)
{
    std::string_view result = current;
    for (; steps != 0; steps += steps > 0 ? -1 : 1)
    {
        const int height = RenderHeight(result, picture_height);
        std::string_view next;
        int next_height = 0;
        for (const auto value : kRenderResolutions)
        {
            const int candidate = RenderHeight(value, picture_height);
            const bool closer = steps > 0 ? candidate > height && (next.empty() || candidate < next_height)
                                          : candidate < height && (next.empty() || candidate > next_height);
            if (closer)
            {
                next = value;
                next_height = candidate;
            }
        }
        if (next.empty()) break;
        result = next;
    }
    return result;
}
} // namespace mscharged
