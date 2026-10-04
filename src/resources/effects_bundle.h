#pragma once
#include "resources/binary_reader.h"
#include "resources/texture_bundle.h"
#include <array>
#include <vector>

namespace mscharged::resources
{
struct EffectRange { float base = 0, range = 0; };
struct EffectCurveKey { float time = 0, cubic = 0, quadratic = 0, linear = 0, constant = 0; };
struct EffectProperty
{
    bool curved = false;
    // Meaningful for constant/random properties only; curved properties own keys.
    EffectRange value;
    std::vector<EffectCurveKey> keys;
};
struct EffectTemplate
{
    std::uint32_t hash = 0;
    float fountain_life = 0;
    EffectRange mass, particle_life, inherit_velocity, acceleration, rotation, fps;
    float unidentified_030 = 0;
    std::uint8_t emitter = 0, blend = 0, billboard = 0, flags = 0;
    std::uint32_t texture = 0, model = 0;
    std::int32_t frames = 0;
    std::array<std::uint32_t, 3> unidentified_040{};
    std::array<EffectProperty, 8> properties;
    // Preserve the actual serialized count. Owned R4QE01 uses 25; the current
    // source declares 26 and reads index25. This reader never invents that colour.
    std::vector<std::array<std::uint8_t, 4>> colours;
};
struct EffectSpec
{
    std::uint32_t hash = 0, template_index = 0, attach = 0, joint = 0, joint_binding = 0;
    float delay = 0, joint_velocity = 0, offset = 0;
    std::uint32_t in_front = 0, ground = 0, light = 0;
    std::array<float, 3> local_offset{};
    std::uint32_t terrain = 0, layer = 0;
    float linger_start = 0, linger_end = 0;
    std::int32_t forward_axis = 0;
};
struct EffectGroup
{
    std::uint32_t hash = 0, lingering = 0;
    std::vector<EffectSpec> specs;
    // Retained authored bytes only: no user-effect parser/factory is selected.
    struct UserSource { std::uint32_t chunk_id; std::vector<std::uint8_t> bytes; };
    std::vector<UserSource> user_sources;
};
struct EffectsBundleEntry
{
    std::array<std::uint32_t, 2> unidentified_header{};
    std::vector<EffectTemplate> templates;
    std::vector<EffectGroup> groups;
};
struct EffectsBundle
{
    std::vector<EffectsBundleEntry> entries;
};
// Decodes serialized records according to original EffectsTemplate/EffectsGroup
// loader order. Counts, ranges and template indices are checked before use.
// Obsolete serialized pointers and uninitialized inactive fields are ignored.
// This supplies owned data, not EmissionManager registration or particle rendering.
// Raw enum/flag values remain available, including attachment3 which the current
// eFXBinding declaration does not describe. Consumers must qualify runtime support.
EffectsBundle ReadEffectsBundle(Bytes resident);
// The decompressed nonresident bundle wraps an ordinary PTLG texture dictionary.
// Uses the same checked texture reader as other native assets; no GPU upload.
TextureBundle ReadEffectsTextureBundle(Bytes decompressed_nonresident);
}
