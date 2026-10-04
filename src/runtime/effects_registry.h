#pragma once
#include "resources/effects_bundle.h"
#include "runtime/particle_files.h"
#include <map>
#include <memory>
#include <string>

class EffectsGroup;
class EffectsTemplate;
namespace mscharged
{
// An immutable native bundle registration snapshot, independent of game arenas.
// Groups borrow linked native templates/properties from shared retained storage.
// These are read-only registration records, not ParticleSystem/GL readiness.
// Never pass them to an in-place loader or mutable inventory. The checked
// ParticleSimulation owner qualifies the selected immutable CPU particle profile.
class EffectsRegistry
{
    struct Storage;
    std::shared_ptr<Storage> storage_;
    explicit EffectsRegistry(resources::EffectsBundle decoded);
public:
    using Handle = std::shared_ptr<const EffectsRegistry>;
    static Handle Decode(resources::Bytes resident);
    // Retains the actual four-read result and decodes both texture dictionaries.
    // Geometry remains opaque; no GL inventory or texture handle is fabricated.
    static Handle FromFiles(std::shared_ptr<const ParticleFiles> files);
    ~EffectsRegistry();
    EffectsRegistry(const EffectsRegistry&) = delete;
    EffectsRegistry& operator=(const EffectsRegistry&) = delete;
    std::size_t Entries() const;
    std::size_t Templates() const;
    std::size_t Groups() const;
    std::size_t RegisteredGroups() const;
    const resources::EffectsBundleEntry& Source(std::size_t entry) const;
    // Later original load order wins a repeated group hash. An unavailable
    // later group also shadows older entries, preventing stale success.
    std::shared_ptr<const EffectsGroup> FindGroup(std::uint32_t hash) const;
    std::shared_ptr<const EffectsTemplate> Template(std::size_t entry, std::size_t index) const;
    std::array<std::uint8_t, 4> Colour(std::size_t entry, std::size_t index, std::size_t colour) const;
    const std::map<std::uint32_t, std::string>& UnavailableGroups() const;
    std::shared_ptr<const resources::Texture> FindTexture(std::uint32_t hash) const;
    std::size_t Textures() const;
    // Authored IDs are unchanged. Missing textures, global/white fallback and
    // model inventory lookup require actual qualified GL services later.
    std::shared_ptr<const ParticleFiles> Files() const;
};
}
