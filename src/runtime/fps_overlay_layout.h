#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mscharged::runtime {

enum class OverlayCorner { TopLeft, TopRight, BottomLeft, BottomRight };

inline std::optional<OverlayCorner> ParseOverlayCorner(std::string_view text)
{
    if (text == "top-left") return OverlayCorner::TopLeft;
    if (text == "top-right") return OverlayCorner::TopRight;
    if (text == "bottom-left") return OverlayCorner::BottomLeft;
    if (text == "bottom-right") return OverlayCorner::BottomRight;
    return std::nullopt;
}

inline constexpr std::array<const char*, 4> kPerformanceLabels = {"FPS", "GPU", "CPU", "LAT"};

inline std::optional<int> OverlayNumber(std::optional<double> value, int maximum)
{
    if (!value || !std::isfinite(*value) || *value < 0) return std::nullopt;
    return static_cast<int>(std::lround(std::min(*value, double(maximum))));
}

inline float FpsOverlayShortSide(std::uint32_t width, std::uint32_t height)
{
    return static_cast<float>(std::min(width, height));
}

// Same size for labels and values; the values use a heavier font weight.
// Work in physical pixels so both normal and high-DPI windows stay sharp.
inline float FpsOverlayFontSize(std::uint32_t width, std::uint32_t height)
{
    return std::max(12.0f, std::round(FpsOverlayShortSide(width, height) * 0.014f));
}

struct FpsOverlayText {
    float label_width;
    float value_width;
    float value_slot; // Reserve the widest value, keeping all four columns still.
};

struct FpsOverlayLayout {
    float left, top, right, bottom;
    float text_y;
    std::array<float, 4> label_x, value_x;
    std::array<float, 3> separator_x;
};

inline FpsOverlayLayout LayoutFpsOverlay(std::uint32_t surface_width, std::uint32_t surface_height,
    OverlayCorner corner, float font_size, const std::array<FpsOverlayText, 4>& text)
{
    const float gap = std::round(font_size * 0.4f);
    const float separator_gap = std::round(font_size * 0.9f);
    const float height = std::round(font_size * 1.4f);
    float width = 6 * separator_gap;
    for (const auto& metric : text) width += std::ceil(metric.label_width) + gap + std::ceil(metric.value_slot);
    const float margin = std::max(8.0f, std::round(FpsOverlayShortSide(surface_width, surface_height) * 0.015f));
    const bool right = corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight;
    const bool bottom = corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight;
    FpsOverlayLayout layout{};
    layout.left = right ? std::max(0.0f, surface_width - margin - width) : margin;
    layout.top = bottom ? std::max(0.0f, surface_height - margin - height) : margin;
    layout.right = layout.left + width;
    layout.bottom = layout.top + height;
    layout.text_y = std::round(layout.top + (height - font_size) * 0.5f);
    float x = layout.left;
    for (std::size_t n = 0; n < text.size(); ++n) {
        layout.label_x[n] = x;
        x += std::ceil(text[n].label_width) + gap + std::ceil(text[n].value_slot);
        layout.value_x[n] = x - std::ceil(text[n].value_width);
        if (n < layout.separator_x.size()) {
            layout.separator_x[n] = x + separator_gap;
            x += 2 * separator_gap;
        }
    }
    return layout;
}

} // namespace mscharged::runtime
