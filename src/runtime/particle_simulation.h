#pragma once
#include "runtime/effects_registry.h"

namespace mscharged
{
struct ParticleSimulationOptions
{
    unsigned capacity = 256;
    std::uint32_t seed = 0x9184eb0c;
};
struct ParticleRenderProfile
{
    std::uint32_t layer;
    std::uint8_t blend;
    bool in_front;
};
struct ParticleSnapshot
{
    float elapsed, fraction, lifespan, rotation, angular_velocity, size, size_scale, velocity, acceleration, mass, frame, fps;
    std::array<float, 3> initial_position, position, direction;
    bool flip_uv;
};
struct ParticleQuad
{
    std::array<std::array<float, 3>, 4> position;
    std::array<std::array<float, 2>, 4> uv;
    std::array<std::uint8_t, 4> colour;
};
// One checked original billboard emitter/spec, not a full EmissionManager.
// Owns original NL particle lists and atlas allocations; requires live arenas.
// Exclusively owns the effects RNG/atlas until Release; other effects RNG users
// must remain inactive during this bounded diagnostic lifetime.
// No GL handle is registered or consumed. Quads retain the actual source texture
// separately so a later renderer can bind real data without a fabricated index.
class ParticleSimulation
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    ParticleSimulation(EffectsRegistry::Handle registry, std::uint32_t group, std::size_t spec,
        ParticleSimulationOptions options = {});
    ~ParticleSimulation();
    ParticleSimulation(const ParticleSimulation&) = delete;
    ParticleSimulation& operator=(const ParticleSimulation&) = delete;
    // Finite dt in [0,1]. Failed arithmetic/qualification poisons this session;
    // Reset reconstructs owned state. No new particles are fabricated on failure.
    bool Advance(float dt);
    void Die();
    void Reset(std::uint32_t seed);
    void Release();
    bool Active() const;
    bool Failed() const;
    float Elapsed() const;
    std::uint32_t Seed() const;
    std::vector<ParticleSnapshot> Snapshot() const;
    // Uses original UV/colour/quad equations for live particles only. View
    // vectors are explicit diagnostic inputs, not a synthetic game camera.
    std::vector<ParticleQuad> Sample(std::array<float, 3> right = {1, 0, 0},
        std::array<float, 3> up = {0, 1, 0});
    std::shared_ptr<const resources::Texture> Texture() const;
    ParticleRenderProfile RenderProfile() const;
};
}
