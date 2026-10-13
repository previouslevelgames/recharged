#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mscharged::runtime {

// display.fps_overlay_corner: where the on-screen frame-rate counter sits.
enum class OverlayCorner { TopLeft, TopRight, BottomLeft, BottomRight };

inline std::optional<OverlayCorner> ParseOverlayCorner(std::string_view text)
{
    if (text == "top-left") return OverlayCorner::TopLeft;
    if (text == "top-right") return OverlayCorner::TopRight;
    if (text == "bottom-left") return OverlayCorner::BottomLeft;
    if (text == "bottom-right") return OverlayCorner::BottomRight;
    return std::nullopt;
}

// Font sizes of the counter in surface pixels: the number, and the smaller
// "FPS" after it. Whole pixels, so the glyphs are drawn at the size they were
// rasterized at. The counter follows the window's shorter side.
struct FpsOverlayFonts {
    float number;
    float label;
};

inline float FpsOverlayShortSide(std::uint32_t surface_width, std::uint32_t surface_height)
{
    return static_cast<float>(std::min(surface_width, surface_height));
}

inline FpsOverlayFonts FpsOverlayFontSizes(std::uint32_t surface_width, std::uint32_t surface_height)
{
    const float number = std::max(14.0f, std::round(FpsOverlayShortSide(surface_width, surface_height) * 0.023f));
    return {number, std::max(11.0f, std::round(number * 0.68f))};
}

// The counter's text, measured at those sizes. Vertical values are below the
// top of the text's line; digits and "FPS" share one baseline.
struct FpsOverlayText {
    float number_width;  // advance width of the digits
    float label_width;   // advance width of "FPS"
    float digit_top;     // top of a digit
    float digit_bottom;  // bottom of a digit (its baseline)
    float number_ascent; // baseline of the number's line
    float label_ascent;  // baseline of the label's line
};

// A translucent pill in one corner, with the number and "FPS" inside: the
// pill and the text lines' top-left corners, in whole surface pixels (y down).
struct FpsOverlayLayout {
    float left, top, right, bottom;
    float rounding;
    float number_x, number_y;
    float label_x, label_y;
};

inline FpsOverlayLayout LayoutFpsOverlay(std::uint32_t surface_width, std::uint32_t surface_height,
                                         OverlayCorner corner, const FpsOverlayFonts& fonts,
                                         const FpsOverlayText& text)
{
    const float height = std::round(fonts.number * 1.3f);
    const float padding = std::round(height * 0.5f);
    const float gap = std::round(fonts.number * 0.22f);
    const float width = std::round(2.0f * padding + text.number_width + gap + text.label_width);
    const float margin = std::max(8.0f, std::round(FpsOverlayShortSide(surface_width, surface_height) * 0.015f));
    const bool right = corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight;
    const bool bottom = corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight;
    const float left = right ? static_cast<float>(surface_width) - margin - width : margin;
    const float top = bottom ? static_cast<float>(surface_height) - margin - height : margin;
    // The digits sit in the middle of the pill; "FPS" shares their baseline.
    const float number_y = std::round(top + 0.5f * (height - text.digit_top - text.digit_bottom));
    const float baseline = number_y + text.number_ascent;
    FpsOverlayLayout layout{};
    layout.left = left;
    layout.top = top;
    layout.right = left + width;
    layout.bottom = top + height;
    layout.rounding = 0.5f * height;
    layout.number_x = left + padding;
    layout.number_y = number_y;
    layout.label_x = std::round(left + padding + text.number_width + gap);
    layout.label_y = std::round(baseline - text.label_ascent);
    return layout;
}

} // namespace mscharged::runtime
