#pragma once
#include "Game/GameObjectLightingData.h"
#include <array>
#include <cstdint>

class LightingLookup;

namespace mscharged
{
// Inputs to the original object-lighting routines. Scene/character/effect
// managers will supply these records when their own startup paths are linked.
// GameObjectLight::useWorldPosition selects position vs authored angles; the
// enabled flag below controls lighting for the complete selected draw.
struct ShadowLighting
{
    const LightingLookup* lookup = nullptr;
    std::uint32_t texture = UINT32_MAX;
    std::array<float, 2> scale{0.042f, 0.073f};
    std::array<float, 2> translation{};
    bool clamp = true;
};

struct GameLighting
{
    bool enabled = false;
    bool double_intensity = false;
    float intensity_scale = 1;
    nlColour ambient{{80, 80, 80, 0}};
    std::array<GameObjectLight, 6> lights{};
    unsigned light_count = 0;
    std::uint32_t ramp_texture = UINT32_MAX;
    ShadowLighting shadow;
};

// The selected decomp's original default stadium key/fill angles and intensities.
// This does not initialize a stadium, camera-relative characters or effect lights.
GameLighting DefaultGameLighting();
void ValidateGameLighting(const GameLighting& lighting);
} // namespace mscharged
