#include "bootstrap/display_shape.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace mscharged;
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try {
        // The game's layout: 16:9 and 4:3 as chosen; auto and stretch take
        // the closer shape to the screen's.
        Check(WidescreenLayout("16:9", 800, 600) && !WidescreenLayout("4:3", 1920, 1080), "Chosen layouts");
        for (const char* aspect : {"auto", "stretch"}) {
            Check(WidescreenLayout(aspect, 1920, 1080) && WidescreenLayout(aspect, 1280, 800)
                      && WidescreenLayout(aspect, 3440, 1440) && WidescreenLayout(aspect, 1600, 1000),
                  "16:9 for 16:9, 16:10 and wider screens");
            Check(!WidescreenLayout(aspect, 1024, 768) && !WidescreenLayout(aspect, 1280, 1024)
                      && !WidescreenLayout(aspect, 1500, 1000) && !WidescreenLayout(aspect, 600, 900),
                  "4:3 for 4:3, 5:4, 3:2 and portrait screens");
            Check(WidescreenLayout(aspect, 0, 0), "An unknown screen takes 16:9");
        }

        // A window keeping the picture's shape: same width.
        const auto wide = SnapToShape(1280, 800, 16, 9);
        Check(wide.width == 1280 && wide.height == 720, "16:10 window snaps to 16:9");
        const auto standard = SnapToShape(1280, 720, 4, 3);
        Check(standard.width == 1280 && standard.height == 960, "16:9 window snaps to 4:3");
        const auto odd = SnapToShape(1366, 768, 16, 9);
        Check(odd.width == 1366 && odd.height == 768, "1366 x 768 already is 16:9 to the pixel");
        Check(SnapToShape(0, 5, 16, 9).height == 5 && SnapToShape(1, 1, 16, 9).height == 1, "Degenerate sizes");

        // Bars around a picture keeping its shape.
        Check(BarsAround(1920, 1080, 16, 9).size == 0, "16:9 on 16:9: no bars");
        const auto deck = BarsAround(1280, 800, 16, 9);
        Check(!deck.sides && deck.size == 40, "Steam Deck: 40 px above and below");
        const auto ultrawide = BarsAround(3440, 1440, 16, 9);
        Check(ultrawide.sides && ultrawide.size == 440, "21:9: 440 px left and right");
        const auto pillar = BarsAround(1920, 1080, 4, 3);
        Check(pillar.sides && pillar.size == 240, "4:3 on 16:9: 240 px left and right");
        Check(BarsAround(0, 1080, 16, 9).size == 0, "Unknown screen: no bars");

        // The picture's own size: without its bars, or the whole screen stretched.
        const auto kept = PictureSize(1280, 800, 16, 9, false);
        const auto filled = PictureSize(1280, 800, 16, 9, true);
        const auto pillared = PictureSize(3440, 1440, 16, 9, false);
        Check(kept.width == 1280 && kept.height == 720 && filled.width == 1280 && filled.height == 800
                  && pillared.width == 2560 && pillared.height == 1440,
              "Picture sizes");

        // A stretched picture's change of shape.
        Check(StretchPercent(1920, 1080, 16, 9) == 0 && StretchPercent(1280, 800, 16, 9) == -10
                  && StretchPercent(3440, 1440, 16, 9) == 34 && StretchPercent(1920, 1080, 4, 3) == 33,
              "Stretch percentages");

        // 3D resolution: scales, heights and hotkey steps.
        Check(RenderScale("window") == 0.0f && RenderScale("native") == 1.0f && RenderScale("2x") == 2.0f
                  && RenderScale("3x") == 3.0f && RenderScale("4x") == 4.0f && RenderScale("other") == 1.0f,
              "Render scales");
        Check(RenderWidth("window", 1920) == 1920 && RenderWidth("window", 320) == kNativeRenderWidth
                  && RenderWidth("2x", 1920) == 1280 && RenderWidth("native", 1920) == 640,
              "Render widths");
        Check(RenderResolutionLabel("window") == "Window" && RenderResolutionLabel("native") == "Native"
                  && RenderResolutionLabel("3x") == "3x",
              "Labels");
        Check(RenderHeight("window", 1080) == 1080 && RenderHeight("window", 300) == kNativeRenderHeight
                  && RenderHeight("3x", 1080) == 1344 && RenderHeight("native", 1080) == 448,
              "Render heights");
        const auto step = [](const char* current, int steps, int height) {
            return std::string(StepRenderResolution(current, steps, height));
        };
        // A 1080p picture: Native 448, 2x 896, Window 1080, 3x 1344, 4x 1792.
        Check(step("window", 1, 1080) == "3x" && step("window", -1, 1080) == "2x", "Window steps between 2x and 3x");
        Check(step("native", 1, 1080) == "2x" && step("2x", 1, 1080) == "window" && step("3x", 1, 1080) == "4x",
              "Steps up");
        Check(step("4x", 1, 1080) == "4x" && step("native", -1, 1080) == "native", "The ends stay");
        Check(step("native", 3, 1080) == "3x" && step("4x", -2, 1080) == "window" && step("2x", 0, 1080) == "2x",
              "Several steps at once");
        // A 4K picture: Window is the highest.
        Check(step("4x", 1, 2160) == "window" && step("window", 1, 2160) == "window"
                  && step("window", -1, 2160) == "4x",
              "Window above 4x on a 4K screen");
        // A small window as high as 2x: Window and 2x draw the same height.
        Check(step("native", 1, 896) == "2x" && step("2x", 1, 896) == "3x" && step("window", 1, 896) == "3x"
                  && step("window", -1, 896) == "native",
              "Equal heights are one step");
        std::cout << checks << " display shape checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
