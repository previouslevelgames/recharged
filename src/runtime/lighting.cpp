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
    result.lights[0].rotYDeg = params.inGameKeyRotYDeg;
    result.lights[0].rotZDeg = params.inGameKeyRotZDeg;
    result.lights[1].intensity = params.inGameFillIntensity;
    result.lights[1].rotYDeg = params.inGameFillRotYDeg;
    result.lights[1].rotZDeg = params.inGameFillRotZDeg;
    return result;
}

void ValidateGameLighting(const GameLighting& lighting)
{
    if (lighting.light_count > lighting.lights.size())
        throw std::invalid_argument("Object lighting supports at most six diffuse lights");
    Bounded(lighting.intensity_scale, 16, "Invalid lighting intensity scale");
    if (lighting.intensity_scale < 0)
        throw std::invalid_argument("Negative lighting intensity scale");
    if (lighting.character && lighting.character->light_count > lighting.character->lights.size())
        throw std::invalid_argument("Character lighting supports at most six explicit lights");
    const auto validate_light = [](const GameObjectLight& light)
    {
        if (light.useColour > 1 || light.isPointLight > 1)
            throw std::invalid_argument("Invalid object light flags");
        Bounded(light.intensity, 16, "Invalid object light intensity");
        if (light.intensity < 0) throw std::invalid_argument("Negative object light intensity");
        Bounded(light.rotYDeg, 36000, "Invalid object light Y angle");
        Bounded(light.rotZDeg, 36000, "Invalid object light Z angle");
        Bounded(light.worldPosition.x, 1e6f, "Invalid object light position");
        Bounded(light.worldPosition.y, 1e6f, "Invalid object light position");
        Bounded(light.worldPosition.z, 1e6f, "Invalid object light position");
        if (light.isPointLight)
        {
            Bounded(light.radius, 1e6f, "Invalid point light radius");
            if (light.radius < 1e-4f) throw std::invalid_argument("Point light radius is too small");
        }
    };
    for (unsigned i = 0; i < lighting.light_count; ++i) validate_light(lighting.lights[i]);
    if (lighting.character)
        for (unsigned i = 0; i < lighting.character->light_count; ++i) validate_light(lighting.character->lights[i]);
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
int IsGameObjectLightingEnabled() { return mscharged::ActiveGameLighting().inputs.enabled; }
int ShouldDoubleGameObjectLighting() { return mscharged::ActiveGameLighting().inputs.double_intensity; }
int ShouldUseGameObjectLightTexture(int) { return mscharged::ActiveGameLighting().inputs.ramp_texture != UINT32_MAX; }
u32 GetGameObjectLightTexture() { return mscharged::ActiveGameLighting().inputs.ramp_texture; }
int GetGameObjectLightCount(bool character, bool)
{
    const auto& lighting = mscharged::ActiveGameLighting().inputs;
    if (character)
    {
        if (!lighting.character) throw std::logic_error("Character lights require explicit native inputs");
        return lighting.character->light_count;
    }
    return lighting.light_count;
}
GameObjectLight* GetGameObjectLight(int index, bool character)
{
    auto& lighting = mscharged::ActiveGameLighting().inputs;
    if (character)
    {
        if (!lighting.character || index < 0 || unsigned(index) >= lighting.character->light_count)
            throw std::out_of_range("Character light is outside the explicit input set");
        return &lighting.character->lights[index];
    }
    if (index < 0 || unsigned(index) >= lighting.light_count)
        throw std::out_of_range("Object light is outside the active input set");
    return &lighting.lights[index];
}
bool IsShadowLookupActive() { return mscharged::ActiveGameLighting().inputs.shadow.lookup != nullptr; }
void SetGameObjectShadowViewMatrix(const nlMatrix4* matrix)
{
    // Original setter in GameObjectLighting.cpp, with per-view native storage.
    // Actual skinned shadow sampling still has its explicit integration gate.
    if (!IsShadowLookupActive()) return;
    if (!matrix) throw std::invalid_argument("Shadow view matrix is absent");
    for (float value : matrix->e)
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite shadow view matrix");
    nlMatrix4 inverse;
    nlInvertMatrix(inverse, *matrix);
    for (float value : inverse.e)
        if (!std::isfinite(value)) throw std::invalid_argument("Non-finite shadow inverse view matrix");
    mscharged::ActiveGameLighting().shadow_inverse_view = inverse;
}
