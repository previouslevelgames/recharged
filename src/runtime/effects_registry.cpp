#include "runtime/effects_registry.h"
#include "Game/Effects/EffectsBundleData.h"
#include "Game/Effects/EffectsGroup.h"
#include "Game/Effects/EffectsTemplate.h"
#include <algorithm>

namespace mscharged
{
namespace
{
fxRange Range(const resources::EffectRange& range) { return {range.base, range.range}; }
struct NativeTemplate
{
    EffectsTemplate value{};
    std::array<fxAnimatedRange, 8> properties{};
    std::array<std::vector<fxCurveKey>, 8> keys;
    explicit NativeTemplate(const resources::EffectTemplate& source)
    {
        value.m_uHashID = source.hash; value.m_fFountainLife = source.fountain_life;
        value.m_rMass = Range(source.mass); value.m_rParticleLife = Range(source.particle_life);
        value.m_rInheritVelocity = Range(source.inherit_velocity); value.m_rAcceleration = Range(source.acceleration);
        value.m_rRotation = Range(source.rotation); value.m_fTexcoordFlipPercentage = source.unidentified_030;
        value.m_eEmitter = source.emitter; value.m_eBlend = source.blend; value.m_eBillboard = source.billboard;
        value.m_uFlags = source.flags; value.m_hTexture = source.texture; value.m_nFrames = source.frames;
        value.m_uEmitterDeathCode = source.unidentified_040[0]; value.m_uParticleCreationCode = source.unidentified_040[1];
        value.m_uParticleDeathCode = source.unidentified_040[2]; value.m_rFPS = Range(source.fps); value.m_uModelID = source.model;
        for (unsigned i = 0; i < properties.size(); ++i)
        {
            const auto& p = source.properties[i]; auto& native = properties[i]; auto& retained = keys[i];
            native.mUseCurve = p.curved; native.base = p.value.base; native.range = p.value.range;
            for (const auto& key : p.keys)
                retained.push_back({key.time, key.cubic, key.quadratic, key.linear, key.constant});
            native.mNumKeys = retained.size(); native.mKeys = retained.empty() ? nullptr : retained.data();
            value.mProperties[i] = &native;
        }
        for (const auto& colour : source.colours)
            value.m_cColour.push_back({{colour[0], colour[1], colour[2], colour[3]}});
    }
};
struct NativeGroup
{
    EffectsGroup value{};
    std::vector<EffectsSpec> specs;
    explicit NativeGroup(const resources::EffectGroup& source)
    {
        // Registration consumes no attachment operation, but storing a value
        // outside an unfixed enum's representable range would itself be unsafe.
        for (const auto& s : source.specs)
            resources::Require(s.attach <= 3 && s.joint_binding <= 3, "Effects binding exceeds its native enum representation");
        value.m_hashID = source.hash; value.m_numSpecs = source.specs.size(); value.m_bIsLingering = source.lingering;
        for (const auto& s : source.specs)
        {
            EffectsSpec spec{};
            spec.m_uHashID = s.hash; spec.m_uTemplateIndex = s.template_index;
            spec.m_eAttach = static_cast<eFXBinding>(s.attach); spec.m_uJointID = s.joint;
            spec.m_fDelay = s.delay; spec.m_eJointBinding = static_cast<eJointBinding>(s.joint_binding);
            spec.m_fJointVelocity = s.joint_velocity; spec.m_bInFront = s.in_front;
            spec.m_bGround = s.ground; spec.m_bLight = s.light; spec.m_fOffset = s.offset;
            spec.m_vLocalOffset = {s.local_offset[0], s.local_offset[1], s.local_offset[2]};
            spec.m_uTerrainID = s.terrain; spec.m_fLingerStart = s.linger_start; spec.m_fLingerEnd = s.linger_end;
            spec.m_uLayer = s.layer; spec.m_nForwardAxis = s.forward_axis;
            specs.push_back(spec);
        }
        value.m_specs = specs.empty() ? nullptr : specs.data();
        // Nonzero user-spec groups are excluded before constructing native
        // storage. This is the original parser's actual zero-user branch.
        value.m_userSpecs = 0; value.m_userSpecsPtr = nullptr; value.mUserSpecSources = nullptr;
    }
};
struct NativeEntry
{
    EffectsBundleData value{};
    std::vector<std::unique_ptr<NativeTemplate>> templates;
    std::vector<std::unique_ptr<NativeGroup>> groups;
    std::vector<EffectsTemplate*> template_table;
    std::vector<EffectsGroup*> group_table;
    explicit NativeEntry(const resources::EffectsBundleEntry& source)
    {
        for (unsigned i = 0; i < 8; ++i)
            value.unknown_0x00[i] = source.unidentified_header[i / 4] >> (24 - i % 4 * 8);
        for (const auto& item : source.templates)
        {
            templates.push_back(std::make_unique<NativeTemplate>(item));
            template_table.push_back(&templates.back()->value);
        }
        for (const auto& item : source.groups)
        {
            if (!item.user_sources.empty()) continue;
            groups.push_back(std::make_unique<NativeGroup>(item));
            group_table.push_back(&groups.back()->value);
        }
        value.mNumTemplates = template_table.size(); value.mTemplates = template_table.empty() ? nullptr : template_table.data();
        value.mNumGroups = group_table.size(); value.mGroups = group_table.empty() ? nullptr : group_table.data();
        // Identical original final initialization pass, on checked host pointers.
        value.ResolveTemplates();
    }
    ~NativeEntry() { value.Destroy(); }
};
}
struct EffectsRegistry::Storage
{
    resources::EffectsBundle source;
    std::vector<std::unique_ptr<NativeEntry>> entries;
    std::map<std::uint32_t, EffectsGroup*> groups;
    std::map<std::uint32_t, std::string> unavailable;
    std::map<std::uint32_t, std::shared_ptr<const resources::Texture>> textures;
    std::shared_ptr<const ParticleFiles> files;
    std::size_t template_count = 0, group_count = 0;
    explicit Storage(resources::EffectsBundle decoded) : source(std::move(decoded))
    {
        for (const auto& entry : source.entries)
        {
            entries.push_back(std::make_unique<NativeEntry>(entry));
            auto& native = *entries.back();
            template_count += entry.templates.size(); group_count += entry.groups.size();
            std::size_t index = 0;
            // Original EffectsBundle::Load replaces an existing hash after Add
            // reports a collision. Preserve file order, not sorted hash order.
            for (const auto& group : entry.groups)
            {
                groups.erase(group.hash); unavailable.erase(group.hash);
                if (!group.user_sources.empty())
                    unavailable.emplace(group.hash, "Original user-effect parser and factories are not selected");
                else groups.emplace(group.hash, &native.groups.at(index++)->value);
            }
        }
    }
};
EffectsRegistry::EffectsRegistry(resources::EffectsBundle decoded)
    : storage_(std::make_shared<Storage>(std::move(decoded))) {}
EffectsRegistry::~EffectsRegistry() = default;
EffectsRegistry::Handle EffectsRegistry::Decode(resources::Bytes resident)
{ return Handle(new EffectsRegistry(resources::ReadEffectsBundle(resident))); }
EffectsRegistry::Handle EffectsRegistry::FromFiles(std::shared_ptr<const ParticleFiles> files)
{
    if (!files) throw std::invalid_argument("Effects registration requires retained particle files");
    std::size_t total = 0;
    for (const auto& file : files->data)
    {
        resources::Require(!file.empty() && file.size() <= resources::MaximumAssetBytes
            && file.size() <= ParticleFileLoad::MaximumRetainedBytes - total, "Effects file snapshot exceeds its batch bounds");
        total += file.size();
    }
    auto registry = std::shared_ptr<EffectsRegistry>(new EffectsRegistry(resources::ReadEffectsBundle((*files)[ParticleFileKind::Resident])));
    const std::array dictionaries{resources::ReadEffectsTextureBundle((*files)[ParticleFileKind::NonResident]),
        resources::ReadTextureBundle((*files)[ParticleFileKind::Textures])};
    for (const auto& dictionary : dictionaries)
    {
        if (!dictionary.animations.empty())
            throw resources::UnsupportedResource("Animated effects textures require qualified texture-manager binding");
        for (const auto& texture : dictionary.textures)
        {
            if (const auto found = registry->storage_->textures.find(texture.id); found != registry->storage_->textures.end())
            {
                const auto& old = *found->second;
                // Owned files share one byte-identical texture. Its metadata
                // and payload are independent of asynchronous registration
                // order; conflicting aliases require actual GL pool ordering.
                if (old.width != texture.width || old.height != texture.height || old.gx_format != texture.gx_format
                    || old.levels != texture.levels || old.game_format != texture.game_format || old.bits != texture.bits
                    || old.palette_entries != texture.palette_entries || old.pixels != texture.pixels || old.palette != texture.palette)
                    throw resources::UnsupportedResource("Conflicting effects textures need qualified GL inventory precedence");
                continue;
            }
            registry->storage_->textures.emplace(texture.id, std::make_shared<resources::Texture>(texture));
        }
    }
    registry->storage_->files = std::move(files);
    return registry;
}
std::size_t EffectsRegistry::Entries() const { return storage_->source.entries.size(); }
std::size_t EffectsRegistry::Templates() const { return storage_->template_count; }
std::size_t EffectsRegistry::Groups() const { return storage_->group_count; }
std::size_t EffectsRegistry::RegisteredGroups() const { return storage_->groups.size(); }
const resources::EffectsBundleEntry& EffectsRegistry::Source(std::size_t entry) const { return storage_->source.entries.at(entry); }
std::shared_ptr<const EffectsGroup> EffectsRegistry::FindGroup(std::uint32_t hash) const
{
    const auto found = storage_->groups.find(hash);
    return found == storage_->groups.end() ? nullptr : std::shared_ptr<const EffectsGroup>(storage_, found->second);
}
std::shared_ptr<const EffectsTemplate> EffectsRegistry::Template(std::size_t entry, std::size_t index) const
{ return {storage_, &storage_->entries.at(entry)->templates.at(index)->value}; }
std::array<std::uint8_t, 4> EffectsRegistry::Colour(std::size_t entry, std::size_t index, std::size_t colour) const
{
    const auto& result = storage_->entries.at(entry)->templates.at(index)->value.m_cColour.at(colour);
    return {result.c[0], result.c[1], result.c[2], result.c[3]};
}
const std::map<std::uint32_t, std::string>& EffectsRegistry::UnavailableGroups() const { return storage_->unavailable; }
std::shared_ptr<const resources::Texture> EffectsRegistry::FindTexture(std::uint32_t hash) const
{
    const auto found = storage_->textures.find(hash);
    return found == storage_->textures.end() ? nullptr : found->second;
}
std::size_t EffectsRegistry::Textures() const { return storage_->textures.size(); }
std::shared_ptr<const ParticleFiles> EffectsRegistry::Files() const { return storage_->files; }
}
