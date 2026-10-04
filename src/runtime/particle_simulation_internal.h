#pragma once
#include "runtime/particle_simulation.h"
#include "runtime/graphics_memory.h"
#include "Game/Effects/ParticleSystem.h"
#include <thread>

namespace mscharged
{
// One shared original atlas, free particle pool and effects RNG domain. Native
// owners retain it until all systems and renderer leases have been released.
struct ParticleSimulationContext
{
    unsigned capacity;
    std::thread::id thread = std::this_thread::get_id();
    bool live = false, atlas = false;
    std::uint32_t saved_seed = 0;
    nlDLListSlotPool<Particle*> free;
    Particle* particles = nullptr;
    explicit ParticleSimulationContext(ParticleSimulationOptions);
    ~ParticleSimulationContext();
    void Check() const;
};
}
