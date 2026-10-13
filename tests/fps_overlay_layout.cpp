#include "runtime/fps_overlay_layout.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using namespace mscharged::runtime;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

bool Whole(float value) { return value == std::round(value); }

int main()
{
    try {
        Check(ParseOverlayCorner("top-left") == OverlayCorner::TopLeft, "top-left");
        Check(ParseOverlayCorner("top-right") == OverlayCorner::TopRight, "top-right");
        Check(ParseOverlayCorner("bottom-left") == OverlayCorner::BottomLeft, "bottom-left");
        Check(ParseOverlayCorner("bottom-right") == OverlayCorner::BottomRight, "bottom-right");
        Check(!ParseOverlayCorner("center") && !ParseOverlayCorner("Top-Right") && !ParseOverlayCorner(""),
              "Other corners are rejected");

        // The counter follows the window's shorter side, in whole pixels.
        const auto hd = FpsOverlayFontSizes(1280, 720), full = FpsOverlayFontSizes(1920, 1080);
        const auto uhd = FpsOverlayFontSizes(3840, 2160);
        Check(hd.number == 17 && hd.label == 12, "720p font sizes");
        Check(full.number == 25 && full.label == 17, "1080p font sizes");
        Check(uhd.number == 50 && uhd.label == 34, "2160p font sizes");
        const auto portrait = FpsOverlayFontSizes(1080, 1920), square4x3 = FpsOverlayFontSizes(1440, 1080);
        Check(portrait.number == 25 && square4x3.number == 25, "Portrait and 4:3 windows use their shorter side");
        const auto tiny = FpsOverlayFontSizes(320, 200);
        Check(tiny.number == 14 && tiny.label == 11, "Small windows keep a readable minimum");

        // Measured like Roboto at 25/17 pixels: "60" and "FPS".
        FpsOverlayText text{};
        text.number_width = 24;
        text.label_width = 30;
        text.digit_top = 6;
        text.digit_bottom = 21;
        text.number_ascent = 21;
        text.label_ascent = 15;
        const auto exact = LayoutFpsOverlay(1920, 1080, OverlayCorner::TopRight, full, text);
        Check(exact.left == 1810 && exact.top == 16 && exact.right == 1904 && exact.bottom == 49,
              "1080p top-right pill");
        Check(exact.rounding == 16.5f, "The pill's ends are half circles");
        Check(exact.number_x == 1827 && exact.number_y == 19 && exact.label_x == 1857 && exact.label_y == 25,
              "1080p top-right text");

        const std::uint32_t sizes[][2] = {{1280, 720}, {1920, 1080}, {3840, 2160}, {960, 540}, {600, 900}};
        const OverlayCorner corners[] = {OverlayCorner::TopLeft, OverlayCorner::TopRight, OverlayCorner::BottomLeft,
                                         OverlayCorner::BottomRight};
        for (const auto& size : sizes)
            for (const auto corner : corners) {
                const auto fonts = FpsOverlayFontSizes(size[0], size[1]);
                const float scale = fonts.number / 25.0f;
                FpsOverlayText scaled{};
                scaled.number_width = std::round(text.number_width * scale);
                scaled.label_width = std::round(text.label_width * scale);
                scaled.digit_top = std::round(text.digit_top * scale);
                scaled.digit_bottom = std::round(text.digit_bottom * scale);
                scaled.number_ascent = std::round(text.number_ascent * scale);
                scaled.label_ascent = std::round(text.label_ascent * fonts.label / 17.0f);
                const auto layout = LayoutFpsOverlay(size[0], size[1], corner, fonts, scaled);
                const float margin = std::fmax(8.0f, std::round(std::fmin(size[0], size[1]) * 0.015f));
                const bool right = corner == OverlayCorner::TopRight || corner == OverlayCorner::BottomRight;
                const bool bottom = corner == OverlayCorner::BottomLeft || corner == OverlayCorner::BottomRight;
                Check(Whole(layout.left) && Whole(layout.top) && Whole(layout.right) && Whole(layout.bottom)
                          && Whole(layout.number_x) && Whole(layout.number_y) && Whole(layout.label_x)
                          && Whole(layout.label_y),
                      "Whole pixels keep the text sharp");
                Check(right ? layout.right == size[0] - margin : layout.left == margin, "Horizontal corner margin");
                Check(bottom ? layout.bottom == size[1] - margin : layout.top == margin, "Vertical corner margin");
                Check(layout.left >= 0 && layout.top >= 0 && layout.right <= size[0] && layout.bottom <= size[1],
                      "The pill stays in the window");
                Check(layout.number_x >= layout.left + layout.rounding * 0.9f
                          && layout.label_x + scaled.label_width <= layout.right - layout.rounding * 0.9f,
                      "Text stays clear of the round ends");
                Check(layout.label_x >= layout.number_x + scaled.number_width, "FPS follows the number");
                const float digits = layout.number_y + 0.5f * (scaled.digit_top + scaled.digit_bottom);
                Check(std::fabs(digits - 0.5f * (layout.top + layout.bottom)) <= 0.5f, "Digits centred in the pill");
                Check(std::fabs((layout.number_y + scaled.number_ascent) - (layout.label_y + scaled.label_ascent)) <= 0.5f,
                      "Number and FPS share a baseline");
            }
        std::cout << checks << " frame-rate counter layout checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
