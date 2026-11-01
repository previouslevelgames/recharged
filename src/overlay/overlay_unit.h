#pragma once

#include <algorithm>

namespace mscharged::overlay
{
// The overlay's smallest text: captions, 13 dp, kept at least this many
// pixels tall in small windows.
inline constexpr float kOverlayCaptionDp = 13.0f;
inline constexpr float kOverlayMinCaptionPixels = 11.0f;
// The window's smallest size (overlay_frame.cpp: 640 x 420 dp) with its margin.
inline constexpr float kOverlayMinWindowDp[2] = {640.0f + 32.0f, 420.0f + 32.0f};

// The picture's height in dp: the overlay's window takes the same share of
// the picture at any size, its contents drawn finer than the game's 720
// lines.
inline constexpr float kOverlayPictureDp = 900.0f;

// Pixels per dp for the overlay (OverlayStyle::unit) over a picture of
// `width` x `height` pixels: its height / 900, so a 1920x1080 window draws at
// 1.2 and a 4K one at 2.4; but in small windows not below the scale that keeps
// the captions readable (11 pixels), as long as the overlay's smallest window
// still fits there (the window then takes more of the picture); from 0.5 to 4.
inline float OverlayUnit(float width, float height)
{
    const float readable = kOverlayMinCaptionPixels / kOverlayCaptionDp;
    const float fits = std::min(width / kOverlayMinWindowDp[0], height / kOverlayMinWindowDp[1]);
    return std::clamp(std::max(height / kOverlayPictureDp, std::min(readable, fits)), 0.5f, 4.0f);
}
} // namespace mscharged::overlay
