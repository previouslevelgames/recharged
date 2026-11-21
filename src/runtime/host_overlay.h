#pragma once

#include "runtime/fps_overlay_layout.h"
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
#include "ui/rui_kit.h"
#endif
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

class HostMenus;

// What the host draws over the presented picture, in windowed and fullscreen
// mode: the performance overlay (display.fps_overlay: FPS | GPU % | CPU %, then
// in network sessions LAT (round trip, ms) and in network matches with rollback
// DLY (input delay, ms) | RB (rollback frames)), short messages, such as what a display hotkey changed, and
// Recharged's menus. Host presentation only; the game never sees it. Use it on the presentation owner thread,
// between game frames.
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
    // The performance numbers in `corner`, or none (the game's Settings switch them).
    void SetPerformance(std::optional<OverlayCorner> corner);
    // The network numbers: none outside network sessions (LAT, DLY and RB
    // hidden), a latency alone in one, all three in a match with rollback.
    struct Network {
        bool session = false;
        std::optional<double> latency_ms;
        bool rollback = false;
        std::optional<double> delay_ms;
        std::optional<double> rollback_frames;
    };
    void SetNetwork(const Network& network);
    // Shows `text` for a moment, replacing the message shown before.
    void ShowMessage(std::string text, Clock::time_point now);
    // A lasting hint in the top-left corner (the menus' key while the game is
    // in its menus): its keys as keycaps, then `text`. Hidden while the menus
    // show; messages show below it. None hides it.
    struct Hint {
        std::vector<std::string> keys;
        std::string text;
        bool operator==(const Hint&) const = default;
    };
    void SetHint(std::optional<Hint> hint);
    // Recharged's menus, drawn while shown under the performance numbers and
    // messages; null for none.
    void SetMenus(HostMenus* menus);
    // Rebuild only when a displayed value, the message's fade or the window's
    // pixel size changes, and every call while the menus show. Unknown
    // measurements show N/A, including during the first sample window.
    void Update(std::uint32_t surface_width, std::uint32_t surface_height, Clock::time_point now);

private:
    void BuildFonts(float size, float menuScale);

    std::optional<OverlayCorner> performance_;
    std::optional<platform::SystemUsageMonitor> usage_;
    std::vector<unsigned char> fontData_;
    std::optional<int> value_;
    Network network_;
    std::string message_;
    std::optional<Hint> hint_, shownHint_;
    HostMenus* menus_ = nullptr;
    bool shownMenus_ = false;
    Clock::time_point messageShown_{};
    std::array<std::optional<int>, kMaxPerformanceMetrics> shownValues_{};
    std::size_t shownCount_ = 0;
    std::string shownMessage_;
    int shownOpacity_ = 0;
    bool drawn_ = false;
    std::uint32_t shownWidth_ = 0;
    std::uint32_t shownHeight_ = 0;
    float builtSize_ = 0.0f;
    float builtMenuScale_ = 0.0f;
    ImFont* number_ = nullptr;
    ImFont* label_ = nullptr;
    ImFont* messageFont_ = nullptr;
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
    // Recharged's menus' (bold): their title, plates, rows and small lines.
    ui::Fonts menuFonts_{};
#endif
};

} // namespace mscharged::runtime
