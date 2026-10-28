#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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

// FPS, GPU and CPU always; LAT in network sessions; DLY (input delay) and RB
// (rollback frames) in network matches with rollback.
inline constexpr std::array<const char*, 6> kPerformanceLabels = {"FPS", "GPU", "CPU", "LAT", "DLY", "RB"};
inline constexpr std::size_t kMaxPerformanceMetrics = kPerformanceLabels.size();

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
    float value_slot; // Reserve the widest value, keeping the columns still.
};

struct FpsOverlayLayout {
    float left, top, right, bottom;
    float text_y;
    // The first `count` entries: the metrics shown (indices into
    // kPerformanceLabels), left to right, and the dividers between them.
    std::size_t count;
    std::array<std::size_t, kMaxPerformanceMetrics> metric;
    std::array<float, kMaxPerformanceMetrics> label_x, value_x;
    std::array<float, kMaxPerformanceMetrics - 1> separator_x;
};

// `text`: the first `count` metrics of kPerformanceLabels (1 to
// kMaxPerformanceMetrics). All of them when they fit the window; else GPU goes,
// then CPU (a network match's own numbers matter more there), then the last
// ones. Which ones fit depends on the labels and the value slots only, so the
// choice stays as the values change.
inline FpsOverlayLayout LayoutFpsOverlay(std::uint32_t surface_width, std::uint32_t surface_height,
    OverlayCorner corner, float font_size, const FpsOverlayText* text, std::size_t count)
{
    const float gap = std::round(font_size * 0.4f);
    const float separator_gap = std::round(font_size * 0.9f);
    const float height = std::round(font_size * 1.4f);
    const float margin = std::max(8.0f, std::round(FpsOverlayShortSide(surface_width, surface_height) * 0.015f));
    FpsOverlayLayout layout{};
    layout.count = std::clamp<std::size_t>(count, 1, kMaxPerformanceMetrics);
    for (std::size_t n = 0; n < layout.metric.size(); ++n) layout.metric[n] = n;
    const auto columnWidth = [&](std::size_t metric) {
        return std::ceil(text[metric].label_width) + gap + std::ceil(text[metric].value_slot);
    };
    const auto shownWidth = [&] {
        float width = 2 * float(layout.count - 1) * separator_gap;
        for (std::size_t n = 0; n < layout.count; ++n) width += columnWidth(layout.metric[n]);
        return width;
    };
    const auto fits = [&] { return shownWidth() <= float(surface_width) - 2 * margin; };
    const auto drop = [&](std::size_t metric) {
        const auto shown = layout.metric.begin() + layout.count;
        const auto kept = std::remove(layout.metric.begin(), shown, metric);
        if (kept != shown) {
            std::fill(kept, shown, metric);
            layout.count = std::size_t(kept - layout.metric.begin());
        }
    };
    if (!fits()) drop(1); // GPU
    if (!fits()) drop(2); // CPU
    while (!fits() && layout.count > 1) --layout.count;
    const float width = shownWidth();
    const bool right = corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight;
    const bool bottom = corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight;
    layout.left = right ? std::max(0.0f, surface_width - margin - width) : margin;
    layout.top = bottom ? std::max(0.0f, surface_height - margin - height) : margin;
    layout.right = layout.left + width;
    layout.bottom = layout.top + height;
    layout.text_y = std::round(layout.top + (height - font_size) * 0.5f);
    float x = layout.left;
    for (std::size_t n = 0; n < layout.count; ++n) {
        const std::size_t metric = layout.metric[n];
        layout.label_x[n] = x;
        x += columnWidth(metric);
        layout.value_x[n] = x - std::ceil(text[metric].value_width);
        if (n + 1 < layout.count) {
            layout.separator_x[n] = x + separator_gap;
            x += 2 * separator_gap;
        }
    }
    return layout;
}

// Messages read a little larger than the performance overlay's numbers.
inline float MessageFontSize(float overlay_font_size)
{
    return std::round(overlay_font_size * 1.25f);
}

// A short message (what a hotkey changed) on a dark panel in the top-left
// corner, below the performance overlay when that has the same corner.
struct OverlayMessageLayout {
    float left, top, right, bottom;
    float text_x, text_y;
};

inline OverlayMessageLayout LayoutOverlayMessage(std::uint32_t surface_width, std::uint32_t surface_height,
    float font_size, float text_width, const FpsOverlayLayout* top_left_performance)
{
    const float margin = std::max(8.0f, std::round(FpsOverlayShortSide(surface_width, surface_height) * 0.015f));
    const float padding = std::round(font_size * 0.7f);
    const float height = std::round(font_size * 1.8f);
    OverlayMessageLayout layout{};
    layout.left = margin;
    layout.top = top_left_performance ? top_left_performance->bottom + std::round(font_size * 0.5f) : margin;
    layout.right = layout.left + std::ceil(text_width) + 2 * padding;
    layout.bottom = layout.top + height;
    layout.text_x = layout.left + padding;
    layout.text_y = std::round(layout.top + (height - font_size) * 0.5f);
    return layout;
}

} // namespace mscharged::runtime
