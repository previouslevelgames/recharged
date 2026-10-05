#pragma once
#include "resources/world_effects.h"
#include "runtime/particle_controller.h"
#include <functional>
#include <optional>
namespace mscharged
{
struct WorldEffectFrame
{
    std::array<float,16> matrix;
    bool active, always_visible, rendering_enabled;
    std::optional<std::array<std::array<float,4>,6>> frustum;
};
struct WorldEffectRequest
{
    std::uint64_t owner;
    std::uint32_t object, group;
    bool animated;
    WorldEffectFrame initial;
    // Reads the same retained effect identity at actual controller-update time.
    std::function<WorldEffectFrame()> current;
};
struct WorldEffectEmission { int id=-1; float radius=0; };
class WorldEffectEmitter
{
public:
    virtual ~WorldEffectEmitter()=default;
    virtual WorldEffectEmission Emit(const WorldEffectRequest&)=0;
    // Must detach callbacks and retire this owner's real controllers. Failure
    // retains ownership and requires retry before the world is destroyed.
    virtual void Release(std::uint64_t owner)=0;
};
struct WorldEffectSnapshot
{
    std::uint32_t id, group;
    std::int32_t timing, remaining, emission_id;
    float elapsed, previous, radius;
    bool active;
};
// Source Trigger/Reset/Update scheduling over every decoded effect, in original
// AddStart order. Triggering an absent type genuinely changes nothing. Update
// requires a real emitter; this owner does not imply full World/manager readiness.
// Consumes the game-wide nlDefaultSeed in source order without resetting it;
// the exclusive effects uSeed domain remains a distinct RNG.
class WorldEffects
{
    struct Implementation;std::shared_ptr<Implementation> impl_;
public:
    WorldEffects(resources::WorldEffectData::Handle,std::shared_ptr<WorldEffectEmitter> = {});
    ~WorldEffects();
    WorldEffects(const WorldEffects&)=delete;WorldEffects& operator=(const WorldEffects&)=delete;
    std::size_t Trigger(std::uint32_t type);
    void Reset();
    unsigned Update(float dt);
    void SetActive(std::uint32_t id,bool);
    // Actual current camera planes; callers must refresh them after camera motion.
    void SetView(const std::array<std::array<float,4>,6>& planes,bool rendering_enabled);
    std::vector<WorldEffectSnapshot> Snapshot() const;
    std::uint32_t Seed() const;
    bool Failed() const;
    void Release();
};
// Actual bounded emitter-only ParticleControllers adapter. Requires static world
// records and wholly qualified groups; missing group lookup uses the original
// explicit no-group result. Unsupported registered groups fail. Controllers must
// outlive this service and no renderer lease may be held during emission/release.
class WorldBillboardEffects final:public WorldEffectEmitter
{
    struct Implementation;std::unique_ptr<Implementation> impl_;
public:
    WorldBillboardEffects(EffectsRegistry::Handle,std::shared_ptr<ParticleControllers>);
    ~WorldBillboardEffects();
    WorldEffectEmission Emit(const WorldEffectRequest&) override;
    void Release(std::uint64_t owner) override;
};
}
