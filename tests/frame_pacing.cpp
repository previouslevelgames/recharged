// display.frame_rate (bootstrap/frame_rate.h): its values, and how the runtime
// paces each choice with and without VSync.
#include "bootstrap/frame_rate.h"

#include <cstdio>
#include <stdexcept>

using mscharged::DecideFramePacing;
using mscharged::DescribeFramePacing;
using mscharged::FrameRate;
using mscharged::FrameRateText;
using mscharged::ParseFrameRate;

namespace
{
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

FrameRate Rate(const char* text)
{
    const auto rate = ParseFrameRate(text);
    Check(rate.has_value(), "Valid frame rate rejected");
    Check(FrameRateText(*rate) == text, "Frame rate text does not round-trip");
    return *rate;
}

bool Near(double a, double b) { return a > b - 1e-6 && a < b + 1e-6; }
}

int main()
{
    try
    {
        // Values: original by default, the display's rate, no limit, 30-1000.
        Check(FrameRate{} == Rate("original"), "Default frame rate is not the original");
        Check(Rate("display").kind == FrameRate::Kind::Display && Rate("unlimited").kind == FrameRate::Kind::Unlimited,
            "Named frame rates");
        Check(Rate("30").hz == 30 && Rate("120").hz == 120 && Rate("144").hz == 144 && Rate("1000").hz == 1000,
            "Numeric frame rates");
        for (const char* bad : {"", "0", "29", "1001", "0120", "60.0", "120fps", " 120", "120 ", "Display", "-60",
                 "uncapped", "10000", "+60"})
            Check(!ParseFrameRate(bad), "Invalid frame rate accepted");

        // The original keeps the game's own VI pacing on any display.
        for (bool vsync : {false, true})
            for (double display : {0.0, 60.0, 144.0})
                Check(!DecideFramePacing(FrameRate{}, display, vsync).host_paced, "Original frame rate paced by the host");

        // VSync at or below the requested rate: the display paces alone.
        // Its safety slot is 5% shorter than a refresh, so it never competes.
        auto pacing = DecideFramePacing(Rate("display"), 143.998, true);
        Check(pacing.host_paced && pacing.vsync_paces && pacing.interval_ns == 6613848 && Near(pacing.target_hz, 143.998),
            "Display rate with VSync");
        pacing = DecideFramePacing(Rate("144"), 143.998, true);
        Check(pacing.vsync_paces && pacing.interval_ns == 6613848, "Display-rate cap with VSync started a second clock");
        pacing = DecideFramePacing(Rate("unlimited"), 60.0, true);
        Check(pacing.vsync_paces && pacing.interval_ns == 15873016 && Near(pacing.target_hz, 60.0), "Unlimited with VSync");
        pacing = DecideFramePacing(Rate("unlimited"), 0.0, true);
        Check(pacing.vsync_paces && pacing.interval_ns == 0 && pacing.target_hz == 0.0,
            "Unlimited with VSync on an unknown display");
        // A cap below the display's rate keeps its own slot.
        pacing = DecideFramePacing(Rate("120"), 144.0, true);
        Check(!pacing.vsync_paces && pacing.interval_ns == 8333333 && Near(pacing.target_hz, 120.0),
            "Cap below the display rate with VSync");
        pacing = DecideFramePacing(Rate("120"), 0.0, true);
        Check(!pacing.vsync_paces && pacing.interval_ns == 8333333, "Cap with VSync on an unknown display");

        // Without VSync: the requested slot, or none.
        pacing = DecideFramePacing(Rate("144"), 60.0, false);
        Check(!pacing.vsync_paces && pacing.interval_ns == 6944444, "144 FPS without VSync");
        pacing = DecideFramePacing(Rate("display"), 165.0, false);
        Check(pacing.interval_ns == 6060606 && Near(pacing.target_hz, 165.0), "Display rate without VSync");
        pacing = DecideFramePacing(Rate("display"), 0.0, false);
        Check(pacing.interval_ns == 16666667 && Near(pacing.target_hz, 60.0), "Unknown display rate falls back to 60");
        pacing = DecideFramePacing(Rate("display"), 5000.0, false);
        Check(Near(pacing.target_hz, 60.0), "Implausible display rate trusted");
        pacing = DecideFramePacing(Rate("unlimited"), 144.0, false);
        Check(pacing.host_paced && !pacing.vsync_paces && pacing.interval_ns == 0 && pacing.target_hz == 0.0,
            "Unlimited without VSync");

        // Session log descriptions.
        const auto text = [](const char* rate, double display, bool vsync) {
            const auto parsed = *ParseFrameRate(rate);
            return DescribeFramePacing(parsed, DecideFramePacing(parsed, display, vsync), display);
        };
        Check(text("original", 60.0, true) == "original (the game's own pacing, 60 FPS)", "Original description");
        Check(text("display", 143.998, true) == "match the display: 144 FPS, VSync paces (144 Hz display)",
            "Display description");
        Check(text("120", 144.0, true) == "120 FPS cap: 120 FPS slots (144 Hz display)", "Cap description");
        Check(text("unlimited", 59.951, false) == "unlimited: no limit (60 Hz display, VSync off)",
            "Unlimited description");
        Check(text("unlimited", 0.0, true) == "unlimited: the display's rate, VSync paces (display rate unknown)",
            "Unknown display description");
        Check(text("display", 119.88, false) == "match the display: 119.9 FPS slots (119.9 Hz display)",
            "Fractional rate description");

        std::printf("Frame pacing: %u checks passed.\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
