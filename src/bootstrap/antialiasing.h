#pragma once

#include <array>
#include <string_view>

namespace mscharged
{
// display.antialiasing: how the game's picture is smoothed. Multisampling has
// 4 samples only: WebGPU (Dawn) accepts no other count above one.
struct AntialiasingChoice
{
    const char* value;       // the INI value
    const char* label;       // launcher name
    const char* description; // launcher note on the chosen kind
    unsigned samples;        // multisampling of the game's frame (1 = none)
    bool sample_shading;     // each sample shaded on its own (supersampling)
    bool fxaa;               // FXAA over the finished picture
};

inline constexpr std::array kAntialiasingChoices = {
    AntialiasingChoice{"off", "Off", "No smoothing. The lightest on the graphics card.", 1, false, false},
    AntialiasingChoice{"fxaa", "FXAA",
        "Smooths edges in the finished picture. Nearly free, but softens fine detail and text a little.",
        1, false, true},
    AntialiasingChoice{"4x", "4x MSAA",
        "Smooths the edges of 3D shapes (4x multisampling). Costs some GPU time.", 4, false, false},
    AntialiasingChoice{"4x_fxaa", "4x MSAA + FXAA",
        "4x MSAA, then FXAA for the edges it leaves, such as those inside textures.", 4, false, true},
    AntialiasingChoice{"4x_ssaa", "4x SSAA",
        "Shades all 4 samples of each pixel: also smooths edges inside textures, such as stripes on banners. "
        "Costs the most GPU time.",
        4, true, false},
};

// The INI reader rejects other values; an unknown one draws unsmoothed.
inline const AntialiasingChoice& FindAntialiasing(std::string_view value)
{
    for (const auto& choice : kAntialiasingChoices)
        if (value == choice.value) return choice;
    return kAntialiasingChoices.front();
}
}
