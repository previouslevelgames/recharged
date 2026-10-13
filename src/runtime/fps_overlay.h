#pragma once

#include "runtime/fps_overlay_layout.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

struct ImFont;

namespace mscharged::runtime {

// The on-screen frame-rate counter (display.fps_overlay): the whole frames
// per second in a translucent rounded pill in one corner of the game window,
// drawn by Aurora over the presented picture in windowed and fullscreen mode.
// Host presentation only; the game never sees it. Use it on the presentation
// owner thread, between game frames.
class FpsOverlay {
public:
    // font: a TrueType file (the launcher's Roboto); without it the counter
    // falls back to ImGui's built-in font.
    FpsOverlay(const std::filesystem::path& font, OverlayCorner corner);
    ~FpsOverlay();
    FpsOverlay(const FpsOverlay&) = delete;
    FpsOverlay& operator=(const FpsOverlay&) = delete;

    // A new measurement, in frames per second.
    void SetRate(double rate);
    // Lays the counter out again when its number or the window's pixel size
    // changed; nothing is shown before the first measurement.
    void Update(std::uint32_t surface_width, std::uint32_t surface_height);

private:
    void BuildFonts(const FpsOverlayFonts& sizes);

    std::vector<unsigned char> fontData_;
    OverlayCorner corner_;
    std::optional<int> value_;
    std::optional<int> shownValue_;
    std::uint32_t shownWidth_ = 0;
    std::uint32_t shownHeight_ = 0;
    float builtSize_ = 0.0f;
    ImFont* number_ = nullptr;
    ImFont* label_ = nullptr;
};

} // namespace mscharged::runtime
