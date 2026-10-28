#pragma once

// display.frame_rate: how many frames per second the game draws.
//
// "original" keeps the console's pacing: the game's swap waits for VI
// retraces (59.94 per second in 60 Hz modes, 50 in PAL) and its own 30 FPS
// lock in some cutscenes. Every other value is a port option above it: the
// host presents each frame as the game swaps and paces the next one itself,
// while VI retraces keep their real timing for everything that counts them.
// Matches keep their fixed 50 Hz simulation clock either way; the game blends
// its last two simulation steps for every frame it draws.
//
// "display" follows the refresh rate of the window's display, "unlimited"
// sets no limit (VSync, when on, still holds it to the display), and a number
// caps the rate at that many frames per second.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mscharged
{
struct FrameRate
{
    enum class Kind { Original, Display, Unlimited, Fixed };
    Kind kind = Kind::Original;
    int hz = 0; // Fixed only
    bool operator==(const FrameRate& other) const { return kind == other.kind && hz == other.hz; }
    bool operator!=(const FrameRate& other) const { return !(*this == other); }
};

inline constexpr int kMinFrameRate = 30;
inline constexpr int kMaxFrameRate = 1000;
// Where VSync paces, the host's slot is this much shorter than a refresh.
inline constexpr double kVSyncSlotMargin = 1.05;
// A refresh rate the platform reports for a display; anything else is unknown.
inline bool KnownRefreshRate(double hz) { return hz >= 20.0 && hz <= 2000.0; }

inline std::optional<FrameRate> ParseFrameRate(std::string_view text)
{
    if (text == "original") return FrameRate{};
    if (text == "display") return FrameRate{FrameRate::Kind::Display, 0};
    if (text == "unlimited") return FrameRate{FrameRate::Kind::Unlimited, 0};
    if (text.empty() || text.size() > 4) return std::nullopt;
    int hz = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9') return std::nullopt;
        hz = hz * 10 + (c - '0');
    }
    if (text[0] == '0' || hz < kMinFrameRate || hz > kMaxFrameRate) return std::nullopt;
    return FrameRate{FrameRate::Kind::Fixed, hz};
}

inline std::string FrameRateText(const FrameRate& rate)
{
    switch (rate.kind)
    {
    case FrameRate::Kind::Original: return "original";
    case FrameRate::Kind::Display: return "display";
    case FrameRate::Kind::Unlimited: return "unlimited";
    case FrameRate::Kind::Fixed: return std::to_string(rate.hz);
    }
    return "original";
}

// Network matches with rollback draw at most this many frames per second: a
// rollback replays whole frames, and above 60 its cost (about 9 ms) no longer
// fits in a frame. The simulation keeps its 50 Hz either way.
inline constexpr int kRollbackFrameRate = 60;

// `rate` held to `hz` frames per second on a display (refresh rate in Hz, 0
// when unknown); a rate already at or below it is kept as it is.
inline FrameRate CapFrameRate(const FrameRate& rate, int hz, double display_hz)
{
    switch (rate.kind)
    {
    case FrameRate::Kind::Original: return rate;
    case FrameRate::Kind::Fixed:
        if (rate.hz <= hz) return rate;
        break;
    case FrameRate::Kind::Display:
        if (KnownRefreshRate(display_hz) && display_hz <= hz + 0.5) return rate;
        break;
    case FrameRate::Kind::Unlimited: break;
    }
    return FrameRate{FrameRate::Kind::Fixed, hz};
}

// What the runtime does for a frame rate on a display (refresh rate in Hz, 0
// when unknown) with or without VSync.
struct FramePacing
{
    bool host_paced = false;      // false: the game's own VI retrace pacing
    double target_hz = 0.0;       // frames per second aimed at; 0: no limit
    std::uint64_t interval_ns = 0; // the host's frame slot; 0: no time limit
    bool vsync_paces = false;     // the display's refresh holds presentation
};

inline FramePacing DecideFramePacing(const FrameRate& rate, double display_hz, bool vsync)
{
    FramePacing pacing;
    if (rate.kind == FrameRate::Kind::Original) return pacing;
    pacing.host_paced = true;
    const bool known = KnownRefreshRate(display_hz);
    switch (rate.kind)
    {
    case FrameRate::Kind::Display: pacing.target_hz = known ? display_hz : 60.0; break;
    case FrameRate::Kind::Fixed: pacing.target_hz = rate.hz; break;
    default: pacing.target_hz = 0.0; break;
    }
    // With VSync a slot at (or above) the refresh rate would be a second clock
    // beside the display's: both run at about the same rate but drift in phase,
    // and a slot that ends just after a refresh costs a whole extra refresh.
    // The display paces then; a slot 5% shorter than its refresh never waits
    // while VSync holds the frames, and only bounds them if it does not.
    if (vsync && (pacing.target_hz == 0.0 || (known && pacing.target_hz >= display_hz - 0.5)))
    {
        pacing.vsync_paces = true;
        if (!known) return pacing;
        pacing.target_hz = display_hz;
        pacing.interval_ns = static_cast<std::uint64_t>(std::llround(1e9 / (display_hz * kVSyncSlotMargin)));
        return pacing;
    }
    if (pacing.target_hz > 0.0)
        pacing.interval_ns = static_cast<std::uint64_t>(std::llround(1e9 / pacing.target_hz));
    return pacing;
}

// For the session log, e.g. "144 FPS (the display's refresh rate, VSync paces)".
inline std::string DescribeFramePacing(const FrameRate& rate, const FramePacing& pacing, double display_hz)
{
    if (!pacing.host_paced) return "original (the game's own pacing, 60 FPS)";
    const auto fps = [](double hz) {
        const double rounded = std::round(hz * 10.0) / 10.0;
        std::string text = std::to_string(rounded);
        text.erase(text.find('.') + (rounded == std::floor(rounded) ? 0 : 2));
        return text + " FPS";
    };
    const std::string display = KnownRefreshRate(display_hz) ? fps(display_hz).substr(0, fps(display_hz).size() - 4) + " Hz display"
                                                               : "display rate unknown";
    std::string text;
    switch (rate.kind)
    {
    case FrameRate::Kind::Display: text = "match the display"; break;
    case FrameRate::Kind::Unlimited: text = "unlimited"; break;
    default: text = fps(rate.hz) + " cap"; break;
    }
    if (pacing.vsync_paces)
        return text + ": " + (pacing.target_hz > 0.0 ? fps(pacing.target_hz) : std::string("the display's rate"))
            + ", VSync paces (" + display + ")";
    if (pacing.interval_ns == 0) return text + ": no limit (" + display + ", VSync off)";
    return text + ": " + fps(pacing.target_hz) + " slots (" + display + ")";
}
}
