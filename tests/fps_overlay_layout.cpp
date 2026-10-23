#include "runtime/fps_overlay_layout.h"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace mscharged::runtime;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try {
        Check(ParseOverlayCorner("top-left") == OverlayCorner::TopLeft, "top-left");
        Check(ParseOverlayCorner("top-right") == OverlayCorner::TopRight, "top-right");
        Check(ParseOverlayCorner("bottom-left") == OverlayCorner::BottomLeft, "bottom-left");
        Check(ParseOverlayCorner("bottom-right") == OverlayCorner::BottomRight, "bottom-right");
        Check(!ParseOverlayCorner("center") && !ParseOverlayCorner("Top-Right") && !ParseOverlayCorner(""),
              "Other corners rejected");
        Check(OverlayNumber(59.6, 999) == 60 && OverlayNumber(0, 100) == 0, "Measured values round; zero is valid");
        Check(OverlayNumber(123, 100) == 100 && OverlayNumber(1e100, 999) == 999, "Clamp before integer conversion");
        Check(!OverlayNumber(std::nullopt, 100) && !OverlayNumber(-1, 100)
            && !OverlayNumber(std::numeric_limits<double>::infinity(), 999)
            && !OverlayNumber(std::numeric_limits<double>::quiet_NaN(), 999), "Unknown/invalid measurements stay N/A");
        Check(FpsOverlayFontSize(1920, 1080) == 15 && FpsOverlayFontSize(3840, 2160) == 30,
              "Physical font size follows display resolution");
        Check(FpsOverlayFontSize(320, 200) == 12, "Small windows retain readable text");

        const std::uint32_t sizes[][2] = {
            {320, 200}, {960, 540}, {1280, 720}, {1920, 1080}, {3840, 2160}, {600, 900}, {1440, 1080}};
        const OverlayCorner corners[] = {OverlayCorner::TopLeft, OverlayCorner::TopRight,
                                         OverlayCorner::BottomLeft, OverlayCorner::BottomRight};
        for (const auto& size : sizes)
            for (const auto corner : corners) {
                const float font = FpsOverlayFontSize(size[0], size[1]), scale = font / 15.0f;
                // FPS, GPU, CPU; LAT in a network session; DLY and RB with rollback.
                for (const std::size_t count : {std::size_t{3}, std::size_t{4}, kMaxPerformanceMetrics}) {
                    std::array<FpsOverlayText, kMaxPerformanceMetrics> text = {{{25 * scale, 17 * scale, 26 * scale},
                        {28 * scale, 25 * scale, 36 * scale}, {28 * scale, 17 * scale, 36 * scale},
                        {25 * scale, 26 * scale, 26 * scale}, {27 * scale, 17 * scale, 26 * scale},
                        {20 * scale, 8 * scale, 26 * scale}}};
                    const auto layout = LayoutFpsOverlay(size[0], size[1], corner, font, text.data(), count);
                    Check(layout.count >= 1 && layout.count <= count, "The metrics that fit are laid out");
                    if (size[0] >= 960) Check(layout.count == count, "A wide window shows every metric");
                    // FPS stays first and LAT, DLY and RB outlast GPU and CPU.
                    Check(layout.metric[0] == 0, "FPS first");
                    for (std::size_t n = 0; n + 1 < layout.count; ++n)
                        Check(layout.metric[n] < layout.metric[n + 1], "The metrics keep their order");
                    if (count == kMaxPerformanceMetrics && size[0] == 320)
                        Check(layout.count == 4 && layout.metric[1] == 3 && layout.metric[2] == 4
                            && layout.metric[3] == 5, "A small window keeps FPS and the network's numbers");
                    const float margin = std::max(8.0f, std::round(std::min(size[0], size[1]) * 0.015f));
                    const bool right = corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight;
                    const bool bottom = corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight;
                    Check(right ? layout.right == size[0] - margin : layout.left == margin, "Horizontal corner margin");
                    Check(bottom ? layout.bottom == size[1] - margin : layout.top == margin, "Vertical corner margin");
                    Check(layout.left >= 0 && layout.top >= 0 && layout.right <= size[0] && layout.bottom <= size[1],
                          "All metrics stay in the window");
                    for (std::size_t n = 0; n < layout.count; ++n) {
                        const auto& metric = text[layout.metric[n]];
                        Check(layout.label_x[n] == std::round(layout.label_x[n])
                            && layout.value_x[n] == std::round(layout.value_x[n])
                            && layout.text_y == std::round(layout.text_y), "Whole pixel glyph positions");
                        Check(layout.value_x[n] >= layout.label_x[n] + metric.label_width, "Labels precede their values");
                        if (n + 1 < layout.count)
                            Check(layout.separator_x[n] > layout.value_x[n] + metric.value_width
                                && layout.separator_x[n] < layout.label_x[n + 1], "Dividers separate metric groups");
                    }
                    for (auto& metric : text) metric.value_width = metric.value_slot;
                    const auto widest = LayoutFpsOverlay(size[0], size[1], corner, font, text.data(), count);
                    Check(layout.count == widest.count && layout.metric == widest.metric
                        && layout.left == widest.left && layout.right == widest.right
                        && layout.label_x == widest.label_x && layout.separator_x == widest.separator_x,
                        "Changing digit counts or N/A never shifts columns");
                    // A hotkey's message: top-left, below the performance overlay
                    // when that takes the same corner.
                    const bool shared = corner == OverlayCorner::TopLeft;
                    const float words = 180 * scale;
                    const auto message
                        = LayoutOverlayMessage(size[0], size[1], font, words, shared ? &layout : nullptr);
                    Check(message.left == margin && (shared ? message.top > layout.bottom : message.top == margin),
                          "Message corner");
                    Check(message.text_x > message.left && message.right >= message.text_x + words
                        && message.text_y > message.top && message.text_y + font < message.bottom
                        && message.text_y == std::round(message.text_y), "Message text inside its panel");
                }
            }
        std::cout << checks << " host overlay layout checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
