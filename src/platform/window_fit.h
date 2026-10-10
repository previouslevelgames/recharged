#pragma once

#include <algorithm>

namespace mscharged::platform
{
// Window sizes are logical units: points on macOS (a 1920 x 1080 window is
// 3840 x 2160 pixels on Retina), scaled pixels with Windows display scaling.
struct WindowSize
{
    int width;
    int height;
};

// Room for the title bar and window frame inside a display's usable area
// (which already excludes menu bar, dock and taskbar).
inline constexpr int kWindowFrameWidth = 16;
inline constexpr int kWindowFrameHeight = 48;

// The largest size with the shape of width x height that fits a display whose
// usable area is usable_width x usable_height, including the window frame.
// Sizes that already fit are returned unchanged; results stay even.
inline WindowSize FitWindowSize(int width, int height, int usable_width, int usable_height)
{
    const int room_width = usable_width - kWindowFrameWidth, room_height = usable_height - kWindowFrameHeight;
    if (width <= 0 || height <= 0 || room_width <= 0 || room_height <= 0) return {width, height};
    if (width <= room_width && height <= room_height) return {width, height};
    const double scale = std::min(double(room_width) / width, double(room_height) / height);
    return {std::max(2, int(width * scale) & ~1), std::max(2, int(height * scale) & ~1)};
}
}
