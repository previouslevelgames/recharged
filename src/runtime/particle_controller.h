#pragma once
#include "runtime/particle_simulation.h"
#include <functional>

namespace mscharged
{
class ParticleControllerRenderer;
struct ParticleControllerOptions
{
    bool add_to_end = true;
    std::uint16_t id = 0;
};
struct ParticleControllersOptions
{
    unsigned capacity = 256, max_controllers = 64;
    std::uint32_t seed = 0x9184eb0c;
};
struct ParticleControllerSnapshot
{
    std::uint64_t token;
    std::uint32_t group;
    std::uint16_t id;
    float age;
    bool dying, lingering;
    unsigned systems, particles;
    ParticleEmitterFrame frame;
};
// Bounded, retained emitter-only controller groups. Shares the original pool,
// atlas and effects RNG; it is not the EmissionManager singleton. Entire selected
// groups are qualified before publication. No pose/user/model/light/replay stub.
class ParticleControllers
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    friend class ParticleControllerRenderer;
    struct RenderEntry
    {
        std::shared_ptr<ParticleSimulation> simulation;
        std::uint64_t token;
        bool visible;
    };
    std::vector<RenderEntry> RetainRenderers();
    std::vector<RenderEntry> RenderEntries() const;
    void BeginRenderFrame();
    void BeginRenderDrain();
    void EndRenderFrame();
    void FailRenderFrame();
    void ReleaseRenderers();
public:
    using Token = std::uint64_t;
    using UpdateCallback = std::function<void(ParticleEmitterFrame&)>;
    using FinishedCallback = std::function<void(Token, int)>;
    explicit ParticleControllers(ParticleControllersOptions = {});
    ~ParticleControllers();
    ParticleControllers(const ParticleControllers&) = delete;
    ParticleControllers& operator=(const ParticleControllers&) = delete;
    Token Start(EffectsRegistry::Handle, std::uint32_t group, ParticleControllerOptions = {});
    void SetFrame(Token, const ParticleEmitterFrame&);
    void SetCallbacks(Token, UpdateCallback, FinishedCallback);
    bool Advance(float dt);
    void Stop(Token);
    void Clear(Token);
    void Destroy(Token);
    // Requires released renderers; destroys all controllers and invalidates
    // tokens. Callback exceptions propagate after cleanup. Recreate groups and
    // renderer bindings explicitly after Reset.
    void Reset(std::uint32_t seed);
    void Release();
    bool Active() const;
    bool Failed() const;
    std::uint32_t Seed() const;
    std::vector<ParticleControllerSnapshot> Snapshot() const;
    std::vector<ParticleSnapshot> Particles(Token, std::size_t spec) const;
};
}
