#include "runtime/effects_registry.h"
#include "Game/Effects/EffectsGroup.h"
#include "Game/Effects/EffectsTemplate.h"
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>

using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool good, const char* message) { ++checks; if (!good) throw std::runtime_error(message); }
template<class F> void Reject(F action, std::source_location at = std::source_location::current())
{
    ++checks; try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid effects registration accepted at line " + std::to_string(at.line()));
}
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read effects registry input: " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
bool Bits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
void Inspect(const EffectsRegistry::Handle& registry)
{
    std::size_t templates = 0, groups = 0;
    for (std::size_t e = 0; e < registry->Entries(); ++e)
    {
        const auto& entry = registry->Source(e);
        for (std::size_t i = 0; i < entry.templates.size(); ++i)
        {
            ++templates; const auto& source = entry.templates[i]; const auto value = registry->Template(e, i);
            Check(value->m_uHashID == source.hash && Bits(value->m_fFountainLife, source.fountain_life), "Native template identity/lifetime differs");
            for (const auto& [native, authored] : std::array<std::pair<fxRange, resources::EffectRange>, 6>{{
                {value->m_rMass, source.mass}, {value->m_rParticleLife, source.particle_life},
                {value->m_rInheritVelocity, source.inherit_velocity}, {value->m_rAcceleration, source.acceleration},
                {value->m_rRotation, source.rotation}, {value->m_rFPS, source.fps}}})
                Check(Bits(native.base, authored.base) && Bits(native.range, authored.range), "Native range bits differ");
            Check(Bits(value->m_fTexcoordFlipPercentage, source.unidentified_030) && value->m_eEmitter == source.emitter
                && value->m_eBlend == source.blend && value->m_eBillboard == source.billboard
                && value->m_uFlags == source.flags, "Native template fields differ");
            Check(value->m_hTexture == source.texture && value->m_uModelID == source.model && value->m_nFrames == source.frames,
                "Unqualified resource ID was silently rebound");
            Check(value->m_uEmitterDeathCode == source.unidentified_040[0] && value->m_uParticleCreationCode == source.unidentified_040[1]
                && value->m_uParticleDeathCode == source.unidentified_040[2], "Unknown authored words were changed");
            Check(value->m_cColour.size() == source.colours.size(), "Native colour count was fabricated");
            for (std::size_t c = 0; c < source.colours.size(); ++c)
                Check(registry->Colour(e, i, c) == source.colours[c], "Native colour bytes differ");
            Reject([&] { registry->Colour(e, i, source.colours.size()); });
            for (unsigned p = 0; p < 8; ++p)
            {
                const auto& property = source.properties[p]; const auto* native = value->mProperties[p];
                Check(native && native->mUseCurve == property.curved && native->mNumKeys == property.keys.size(), "Native property lost its key storage");
                if (!property.curved)
                    Check(!native->mKeys && Bits(native->base, property.value.base) && Bits(native->range, property.value.range),
                        "Inactive pointer/count became live or constant bits changed");
                for (unsigned k = 0; k < property.keys.size(); ++k)
                {
                    const auto& a = property.keys[k]; const auto& b = native->mKeys[k];
                    Check(Bits(a.time, b.mTime) && Bits(a.cubic, b.mCubic) && Bits(a.quadratic, b.mQuadratic)
                        && Bits(a.linear, b.mLinear) && Bits(a.constant, b.mConstant), "Native polynomial coefficients differ");
                }
            }
        }
        for (const auto& source : entry.groups)
        {
            ++groups;
            // Lookup is global last-wins; the duplicate fixtures are inspected
            // separately so this pass can compare the original authored index.
            const auto group = registry->FindGroup(source.hash);
            if (!source.user_sources.empty()) continue;
            Check(group && group->m_numSpecs == source.specs.size() && group->m_bIsLingering == source.lingering,
                "Native group metadata differs");
            bool persistent = false;
            for (unsigned i = 0; i < source.specs.size(); ++i)
            {
                const auto& a = source.specs[i]; const auto& b = group->m_specs[i];
                auto target = registry->Template(e, a.template_index);
                Check(b.m_pTemplate == target.get(), "Original group template index resolved to the wrong entry");
                persistent |= entry.templates[a.template_index].fountain_life >= 1e10f
                    || entry.templates[a.template_index].particle_life.base >= 1000.f;
                Check(b.m_uHashID == a.hash && unsigned(b.m_eAttach) == a.attach && b.m_uJointID == a.joint
                    && unsigned(b.m_eJointBinding) == a.joint_binding, "Native attachment metadata differs");
                Check(Bits(b.m_fDelay, a.delay) && Bits(b.m_fJointVelocity, a.joint_velocity)
                    && b.m_bInFront == a.in_front && b.m_bGround == a.ground && b.m_bLight == a.light,
                    "Native spec scalar metadata differs");
                Check(Bits(b.m_fOffset, a.offset) && Bits(b.m_vLocalOffset.x, a.local_offset[0])
                    && Bits(b.m_vLocalOffset.y, a.local_offset[1]) && Bits(b.m_vLocalOffset.z, a.local_offset[2]),
                    "Native spec offset bits differ");
                Check(b.m_uTerrainID == a.terrain && Bits(b.m_fLingerStart, a.linger_start)
                    && Bits(b.m_fLingerEnd, a.linger_end) && b.m_uLayer == a.layer && b.m_nForwardAxis == a.forward_axis,
                    "Native spec tail differs");
            }
            Check(group->IsPersistent() == persistent, "Original persistence query differs from independent thresholds");
            Check(group->m_userSpecs == 0 && !group->m_userSpecsPtr && !group->mUserSpecSources, "User-free group owns fake user specs");
        }
    }
    Check(templates == registry->Templates() && groups == registry->Groups(), "Registry counts differ from authored records");
    Reject([&] { registry->Source(registry->Entries()); });
    Reject([&] { registry->Template(registry->Entries(), 0); });
}
std::shared_ptr<ParticleFiles> Files(const std::filesystem::path& folder)
{
    auto files = std::make_shared<ParticleFiles>();
    const std::array names{"resident.bun", "nonresident.bun", "geometry.bun", "textures.rlt"};
    for (unsigned i = 0; i < 4; ++i) { files->data[i] = Read(folder / names[i]); files->source_sizes[i] = files->data[i].size(); }
    return files;
}
void Generated(const std::filesystem::path& folder)
{
    EffectsGroup unsupported{};
    unsupported.DestroyUserSpecs(); // Original zero-user branch needs no factory.
    unsupported.m_userSpecs = 1;
    Reject([&] { unsupported.DestroyUserSpecs(); });
    auto input = Read(folder / "resident.bun"); auto registry = EffectsRegistry::Decode(input);
    input.assign(input.size(), 0); input.clear(); input.shrink_to_fit(); Inspect(registry);
    Check(registry->Entries() == 1 && registry->Templates() == 2 && registry->Groups() == 1 && registry->RegisteredGroups() == 1,
        "Synthetic source counts differ");
    Check(!registry->FindGroup(0xdead) && !registry->FindTexture(0x12345678) && !registry->Files(), "Missing resource faked registration");
    auto group = registry->FindGroup(0x81f2a311); auto native = registry->Template(0, 0);
    Check(group->m_specs[0].m_pTemplate == registry->Template(0, 1).get()
        && group->m_specs[1].m_pTemplate == native.get(), "Forward/out-of-order authored template indices changed");
    Check(!group->IsPersistent() && native->m_cColour.size() == 25 && registry->Template(0, 1)->m_cColour.size() == 26,
        "Baseline persistence or exact colour count differs");
    Reject([&] { registry->Colour(0, 0, 25); }); Check(registry->Colour(0, 1, 25)[3] == 103, "Authored26th colour was dropped");
    for (auto name : {"persistent-fountain", "persistent-particle"})
    { auto r = EffectsRegistry::Decode(Read(folder / (std::string(name) + ".bun"))); Inspect(r); Check(r->FindGroup(0x81f2a311)->IsPersistent(), "Original persistence threshold failed"); }
    auto duplicate = EffectsRegistry::Decode(Read(folder / "duplicate.bun"));
    Check(duplicate->Entries() == 2 && duplicate->Groups() == 2 && duplicate->RegisteredGroups() == 1
        && duplicate->FindGroup(0x81f2a311)->m_specs[0].m_pTemplate->m_uHashID == 21, "Last original group registration did not replace its hash");
    auto user = EffectsRegistry::Decode(Read(folder / "user.bun"));
    Check(!user->FindGroup(0x81f2a311) && user->UnavailableGroups().size() == 1 && user->RegisteredGroups() == 0
        && user->Source(0).groups[0].user_sources[0].bytes == std::vector<std::uint8_t>({'t','e','s','t'}),
        "Unqualified user group was published or its source was dropped");
    auto shadowed = EffectsRegistry::Decode(Read(folder / "shadowed.bun"));
    Check(!shadowed->FindGroup(0x81f2a311) && shadowed->UnavailableGroups().size() == 1, "Unsupported later group leaked prior success");
    auto restored = EffectsRegistry::Decode(Read(folder / "restored.bun"));
    Check(restored->FindGroup(0x81f2a311) && restored->UnavailableGroups().empty(), "Supported later group did not replace unavailable metadata");
    Reject([&] { EffectsRegistry::Decode(Read(folder / "bad-enum.bun")); });
    auto unbound = EffectsRegistry::Decode(Read(folder / "unbound.bun")); Inspect(unbound);
    Check(unbound->Template(0, 0)->m_hTexture == 0xffffffff && unbound->Template(0, 0)->m_uModelID == 0x12341234,
        "Missing GL services silently substituted a texture/model");
    auto empty = EffectsRegistry::Decode(Read(folder / "empty.bun")); Inspect(empty);
    Check(!empty->Entries() && !empty->Templates() && !empty->Groups() && !empty->RegisteredGroups(), "Empty registry fabricated entries");
    std::weak_ptr<const EffectsGroup> lifetime = group; registry.reset(); native.reset();
    Check(!lifetime.expired() && group->m_specs[0].m_pTemplate->mProperties[1]->mKeys[1].mTime == .5f,
        "Group did not retain its templates/keys after owner disposal");
    group.reset(); Check(lifetime.expired(), "Group/template ownership cycle prevented teardown");
    auto files = Files(folder); auto retained = EffectsRegistry::FromFiles(files); Inspect(retained);
    Check(retained->Textures() == 2 && retained->FindTexture(0x12345678)->pixels[0] == 64
        && retained->Files() == files && !retained->FindTexture(0xffffffff), "Retained file/texture identity differs");
    auto texture = retained->FindTexture(0x12345678); auto last = retained->FindGroup(0x81f2a311);
    files.reset(); retained.reset(); Check(texture->pixels[31] == 95 && last->m_specs[0].m_pTemplate->m_uHashID == 11,
        "Discarded file owner invalidated live registration handles");
    files = Files(folder); files->data[3] = Read(folder / "same.rlt");
    Check(EffectsRegistry::FromFiles(files)->Textures() == 1, "Identical texture alias was not coalesced");
    files->data[3] = Read(folder / "conflict.rlt"); Reject([&] { EffectsRegistry::FromFiles(files); });
    files = Files(folder); files->data[0].pop_back(); Reject([&] { EffectsRegistry::FromFiles(files); });
    files = Files(folder); files->data[1].pop_back(); Reject([&] { EffectsRegistry::FromFiles(files); });
    files = Files(folder); files->data[2].clear(); Reject([&] { EffectsRegistry::FromFiles(files); });
    files = Files(folder); files->data[3].pop_back(); Reject([&] { EffectsRegistry::FromFiles(files); });
    Reject([&] { EffectsRegistry::FromFiles({}); });
    for (unsigned i = 0; i < 16; ++i)
    { auto r = EffectsRegistry::Decode(Read(folder / "resident.bun")); auto g = r->FindGroup(0x81f2a311); r.reset(); Check(!g->IsPersistent(), "Repeated registry teardown damaged links"); }
}
void Owned(const std::filesystem::path& folder)
{
    auto files = std::make_shared<ParticleFiles>();
    const std::array names{"Art/effects/effects.bun", "Art/effects/effectsnonres.bun", "Art/objects/effectsgeometry.bun", "Art/objects/effectsgeometrytextures.rlt"};
    for (unsigned i = 0; i < 4; ++i) { files->data[i] = Read(folder / names[i]); files->source_sizes[i] = files->data[i].size(); }
    auto registry = EffectsRegistry::FromFiles(files); files.reset(); Inspect(registry);
    Check(registry->Entries() == 249 && registry->Templates() == 1745 && registry->Groups() == 249
        && registry->RegisteredGroups() == 249 && registry->UnavailableGroups().empty() && registry->Textures() == 101,
        "Owned effects profile differs");
    unsigned missing_textures = 0, model_templates = 0, persistent = 0;
    for (unsigned i = 0; i < registry->Entries(); ++i)
    {
        for (const auto& t : registry->Source(i).templates)
        { missing_textures += !registry->FindTexture(t.texture); model_templates += t.model != 0xffffffff;
          Check(t.colours.size() == 25, "Owned colour source count changed"); }
        for (const auto& g : registry->Source(i).groups) persistent += registry->FindGroup(g.hash)->IsPersistent();
    }
    std::cout << "Owned: " << registry->RegisteredGroups() << " groups, " << registry->Templates() << " templates, " << persistent
        << " persistent groups, " << registry->Textures() << " unique textures, " << missing_textures << " unresolved template textures, "
        << model_templates << " model-bearing templates; GL/particle consumers remain unselected\n";
}
}
int main(int argc, char** argv)
{
    try
    {
        Check(argc == 2 || argc == 3, "Supply generated folder, or owned folder and --owned");
        if (argc == 3) { Check(std::string_view(argv[2]) == "--owned", "Unknown effects registry mode"); Owned(argv[1]); }
        else Generated(argv[1]);
        std::cout << checks << " effects registry checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
