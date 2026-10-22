#pragma once

#include "runtime/fps_overlay_layout.h"
#include "platform/system_usage.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct ImFont;

namespace mscharged::runtime {

// What the host draws over the presented picture, in windowed and fullscreen
// mode: the performance overlay (display.fps_overlay: FPS | GPU % | CPU % |
// LAT N/A) and short messages, such as what a display hotkey changed. Host
// presentation only; the game never sees it. Use it on the presentation owner
// thread, between game frames.
class HostOverlay {
public:
    using Clock = std::chrono::steady_clock;

    // font: a TrueType file (the launcher's Roboto); without it the overlay
    // falls back to ImGui's built-in font. performance: the performance
    // overlay's corner, or none for messages only.
    HostOverlay(const std::filesystem::path& font, std::optional<OverlayCorner> performance);
    ~HostOverlay();
    HostOverlay(const HostOverlay&) = delete;
    HostOverlay& operator=(const HostOverlay&) = delete;

    // A new measurement, in frames per second.
    void SetRate(double rate);
    // Shows `text` for a moment, replacing the message shown before.
    void ShowMessage(std::string text, Clock::time_point now);
    // Rebuild only when a displayed value, the message's fade or the window's
    // pixel size changes. Unknown measurements show N/A, including during the
    // first sample window.
    void Update(std::uint32_t surface_width, std::uint32_t surface_height, Clock::time_point now);

private:
    void BuildFonts(float size);

    std::optional<OverlayCorner> performance_;
    std::optional<platform::SystemUsageMonitor> usage_;
    std::vector<unsigned char> fontData_;
    std::optional<int> value_;
    std::string message_;
    Clock::time_point messageShown_{};
    std::array<std::optional<int>, 3> shownValues_{};
    std::string shownMessage_;
    int shownOpacity_ = 0;
    bool drawn_ = false;
    std::uint32_t shownWidth_ = 0;
    std::uint32_t shownHeight_ = 0;
    float builtSize_ = 0.0f;
    ImFont* number_ = nullptr;
    ImFont* label_ = nullptr;
    ImFont* messageFont_ = nullptr;
};

} // namespace mscharged::runtime
