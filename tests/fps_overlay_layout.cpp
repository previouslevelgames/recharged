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
                std::array<FpsOverlayText, 4> text = {{{25 * scale, 17 * scale, 26 * scale},
                    {28 * scale, 25 * scale, 36 * scale}, {28 * scale, 17 * scale, 36 * scale},
                    {25 * scale, 26 * scale, 26 * scale}}};
                const auto layout = LayoutFpsOverlay(size[0], size[1], corner, font, text);
                const float margin = std::max(8.0f, std::round(std::min(size[0], size[1]) * 0.015f));
                const bool right = corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight;
                const bool bottom = corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight;
                Check(right ? layout.right == size[0] - margin : layout.left == margin, "Horizontal corner margin");
                Check(bottom ? layout.bottom == size[1] - margin : layout.top == margin, "Vertical corner margin");
                Check(layout.left >= 0 && layout.top >= 0 && layout.right <= size[0] && layout.bottom <= size[1],
                      "All four metrics stay in the window");
                for (std::size_t n = 0; n < text.size(); ++n) {
                    Check(layout.label_x[n] == std::round(layout.label_x[n])
                        && layout.value_x[n] == std::round(layout.value_x[n])
                        && layout.text_y == std::round(layout.text_y), "Whole pixel glyph positions");
                    Check(layout.value_x[n] >= layout.label_x[n] + text[n].label_width, "Labels precede their values");
                    if (n < layout.separator_x.size())
                        Check(layout.separator_x[n] > layout.value_x[n] + text[n].value_width
                            && layout.separator_x[n] < layout.label_x[n + 1], "Dividers separate metric groups");
                }
                for (auto& metric : text) metric.value_width = metric.value_slot;
                const auto widest = LayoutFpsOverlay(size[0], size[1], corner, font, text);
                Check(layout.left == widest.left && layout.right == widest.right
                    && layout.label_x == widest.label_x && layout.separator_x == widest.separator_x,
                    "Changing digit counts or N/A never shifts columns");
            }
        std::cout << checks << " performance overlay checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
