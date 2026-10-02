#include "runtime/lighting_state.h"
#include "runtime/material_environment.h"
#include "Game/GameObjectLighting.h"
#include "Game/Render/LightingLookup.h"
#include "NL/glx/glxTexture.h"
#include <cmath>
#include <optional>
#include <stdexcept>

namespace mscharged
{
namespace
{
std::optional<GameLightingState> active;
void Bounded(float value, float limit, const char* message)
{
    if (!std::isfinite(value) || std::abs(value) > limit)
        throw std::invalid_argument(message);
}
}

GameLighting DefaultGameLighting()
{
    GameLighting result;
    result.enabled = true;
    result.light_count = 2;
    const auto& params = gStadiumGameObjectLightingParams;
    result.lights[0].intensity = params.inGameKeyIntensity;
    result.lights[0].unknown08 = params.inGameKeyRotYDeg;
    result.lights[0].unknown0C = params.inGameKeyRotZDeg;
    result.lights[1].intensity = params.inGameFillIntensity;
    result.lights[1].unknown08 = params.inGameFillRotYDeg;
    result.lights[1].unknown0C = params.inGameFillRotZDeg;
    return result;
}

void ValidateGameLighting(const GameLighting& lighting)
{
    if (lighting.light_count > lighting.lights.size())
        throw std::invalid_argument("Object lighting supports at most six diffuse lights");
    Bounded(lighting.intensity_scale, 16, "Invalid lighting intensity scale");
    if (lighting.intensity_scale < 0)
        throw std::invalid_argument("Negative lighting intensity scale");
    for (unsigned i = 0; i < lighting.light_count; ++i)
    {
        const auto& light = lighting.lights[i];
        if (light.unknown01 > 1 || light.unknown02 > 1)
            throw std::invalid_argument("Invalid object light flags");
        Bounded(light.intensity, 16, "Invalid object light intensity");
        if (light.intensity < 0) throw std::invalid_argument("Negative object light intensity");
        Bounded(light.unknown08, 36000, "Invalid object light Y angle");
        Bounded(light.unknown0C, 36000, "Invalid object light Z angle");
        Bounded(light.worldPosition.x, 1e6f, "Invalid object light position");
        Bounded(light.worldPosition.y, 1e6f, "Invalid object light position");
        Bounded(light.worldPosition.z, 1e6f, "Invalid object light position");
        if (light.unknown02)
        {
            Bounded(light.unknown20, 1e6f, "Invalid point light radius");
            if (light.unknown20 < 1e-4f) throw std::invalid_argument("Point light radius is too small");
        }
    }
    if (lighting.ramp_texture != UINT32_MAX && !glx_GetTex(lighting.ramp_texture))
        throw std::invalid_argument("Lighting ramp is absent from the native inventory");
    const auto& shadow = lighting.shadow;
    for (float value : shadow.scale) Bounded(value, 1e4f, "Invalid shadow lookup scale");
    for (float value : shadow.translation) Bounded(value, 1e4f, "Invalid shadow lookup translation");
    if ((shadow.texture != UINT32_MAX) != (shadow.lookup != nullptr))
        throw std::invalid_argument("Shadow lighting requires both a texture and a loaded lookup");
    if (shadow.lookup && (shadow.lookup->m_NativeTexture != shadow.texture ||
        !shadow.lookup->mValues || shadow.lookup->mWidth <= 0 || shadow.lookup->mHeight <= 0 ||
        !glx_GetTex(shadow.texture)))
        throw std::invalid_argument("Invalid or unloaded shadow lighting lookup");
}

GameLightingState& ActiveGameLighting()
{
    if (!active) throw std::logic_error("Object lighting requires an active material view");
    return *active;
}
void BeginGameLighting(const GameLighting& lighting)
{
    if (active) throw std::logic_error("Nested object lighting context");
    ValidateGameLighting(lighting);
    active.emplace(GameLightingState{lighting});
}
void EndGameLighting() { active.reset(); }
} // namespace mscharged

// Input adapters for the original core. No stadium, character or emission
// manager is linked by this preview; callers supply validated object records.
extern "C"
{
int IsGameObjectLightingEnabled() { return mscharged::ActiveGameLighting().inputs.enabled; }
int ShouldDoubleGameObjectLighting() { return mscharged::ActiveGameLighting().inputs.double_intensity; }
int ShouldUseGameObjectLightTexture(int) { return mscharged::ActiveGameLighting().inputs.ramp_texture != UINT32_MAX; }
u32 GetGameObjectLightTexture() { return mscharged::ActiveGameLighting().inputs.ramp_texture; }
int GetGameObjectLightCount(bool character, bool)
{
    if (character) throw std::logic_error("Character light selection is not integrated");
    return mscharged::ActiveGameLighting().inputs.light_count;
}
GameObjectLight* GetGameObjectLight(s32 index, bool character)
{
    auto& lighting = mscharged::ActiveGameLighting().inputs;
    if (character || index < 0 || unsigned(index) >= lighting.light_count)
        throw std::out_of_range("Object light is outside the active input set");
    return &lighting.lights[index];
}
bool fn_80183C54() { return mscharged::ActiveGameLighting().inputs.shadow.lookup != nullptr; }
}
