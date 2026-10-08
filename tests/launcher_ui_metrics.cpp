#include "launcher/ui_metrics.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace mscharged::launcher;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
} // namespace

int main()
{
    try
    {
        // macOS retina: points are window units, two framebuffer pixels each.
        auto retina = ComputeUiMetrics({2.0f, 2.0f}, 0.0f);
        Require(Near(retina.ui, 1.0f) && Near(retina.density, 2.0f) && Near(retina.font_raster, 2.0f),
                "Retina keeps point layout and rasterizes glyphs at 2x");
        // Windows at 150 %: window units are pixels and content scales 1.5x.
        auto windows = ComputeUiMetrics({1.5f, 1.0f}, 0.0f);
        Require(Near(windows.ui, 1.5f) && Near(windows.density, 1.0f) && Near(windows.font_raster, 1.5f),
                "Windows display scaling enlarges the layout");
        // Wayland fractional scale with high-density windows behaves like retina.
        auto wayland = ComputeUiMetrics({1.25f, 1.25f}, 0.0f);
        Require(Near(wayland.ui, 1.0f) && Near(wayland.font_raster, 1.25f), "Fractional density");
        // Explicit interface size multiplies the content scale.
        auto user = ComputeUiMetrics({1.5f, 1.0f}, 1.25f);
        Require(Near(user.ui, 1.875f), "User interface size multiplies content scale");
        // Automatic sizing shrinks to fit a small display (1280x720 at 200 %).
        auto fitted = ComputeUiMetrics({2.0f, 1.0f}, 0.0f, 1280.0f, 690.0f);
        Require(fitted.ui < 2.0f && fitted.ui >= 0.75f, "Automatic size fits the minimum layout on the display");
        Require(kMinimumDesignHeight * fitted.ui <= 690.0f, "Minimum layout height fits");
        // An explicit size is respected even when it overflows.
        Require(Near(ComputeUiMetrics({2.0f, 1.0f}, 1.0f, 1280.0f, 690.0f).ui, 2.0f), "Explicit size is kept");
        // Invalid SDL values fall back to 1.
        auto invalid = ComputeUiMetrics({NAN, -3.0f}, 0.0f);
        Require(Near(invalid.ui, 1.0f) && Near(invalid.density, 1.0f), "Invalid display values are sanitized");

        Require(ParseUiScaleSetting("auto") == 0.0f, "auto setting");
        Require(Near(ParseUiScaleSetting("150"), 1.5f), "percentage setting");
        Require(ParseUiScaleSetting("74") < 0.0f && ParseUiScaleSetting("201") < 0.0f
            && ParseUiScaleSetting("1.5") < 0.0f && ParseUiScaleSetting("") < 0.0f, "invalid settings rejected");

        auto window = FitWindow(kDefaultDesignWidth, kDefaultDesignHeight, 1.5f, 0.0f, 0.0f);
        Require(window.width == 1680 && window.height == 1080, "Design size scales without bounds");
        window = FitWindow(kDefaultDesignWidth, kDefaultDesignHeight, 1.5f, 1920.0f, 1040.0f);
        Require(window.width <= 1920 && window.height <= 1040, "Window fits the usable area");
        std::puts("Launcher scale metrics passed (retina, Windows scaling, fractional, fit, settings).");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
