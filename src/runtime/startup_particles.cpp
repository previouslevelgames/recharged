#include "runtime/startup.h"
#include "runtime/particle_files.h"
#include "runtime/effects_registry.h"
#include "runtime/particle_simulation.h"
#include "runtime/graphics_memory.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace mscharged
{
std::string VerifyStartupParticleResources()
{
    ParticleFileLoad load;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (load.State() == ParticleFileState::Loading)
    {
        load.Service();
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Particle resource diagnostic timed out");
        if (load.State() == ParticleFileState::Loading)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto files = load.Result();
    const auto registry = EffectsRegistry::FromFiles(files);
    // Fixed authored emitter from the supported USA revision, independently
    // inspected for its no-attachment billboard profile. Never search for a
    // constructor that happens to succeed: missing/changed inputs must fail.
    const auto free1 = StandardAllocator.TotalFreeMemory();
    const auto free2 = VirtualAllocator.TotalFreeMemory();
    std::size_t peak = 0, quads = 0;
    unsigned drain_steps = 0;
    {
        ParticleSimulation simulation(registry, 0xfda2d744, 0);
        for (unsigned i = 0; i < 120; ++i)
        {
            simulation.Advance(1.f / 60);
            const auto frame = simulation.Sample();
            peak = std::max(peak, frame.size());
            quads += frame.size();
        }
        if (!peak || !quads)
            throw std::runtime_error("Authored particle diagnostic emitted no particles");
        simulation.Die();
        bool alive = true;
        while (alive && drain_steps < 120)
        {
            alive = simulation.Advance(1.f / 60);
            quads += simulation.Sample().size();
            ++drain_steps;
        }
        if (alive || !simulation.Snapshot().empty())
            throw std::runtime_error("Authored particle diagnostic did not drain within two seconds");
    }
    if (free1 != StandardAllocator.TotalFreeMemory() || free2 != VirtualAllocator.TotalFreeMemory())
        throw std::runtime_error("Particle simulation did not recover its arena allocations");
    return "Independent particle resource diagnostic: 4 files, " + std::to_string(registry->Templates())
        + " templates, " + std::to_string(registry->Groups()) + " authored groups; "
        + std::to_string(registry->RegisteredGroups()) + " groups resolved through original code, "
        + std::to_string(registry->UnavailableGroups().size()) + " groups require user-effect factories; "
        + std::to_string(registry->Textures()) + " unique textures retained; "
        + std::to_string((*files)[ParticleFileKind::Geometry].size())
        + " geometry bytes retained. Original CPU particle diagnostic fda2d744/0: 120 updates, "
        + std::to_string(peak) + " peak live particles, " + std::to_string(quads)
        + " quad samples, " + std::to_string(drain_steps)
        + " drain updates; both arenas recovered. Geometry/GL registration and particle rendering remain unavailable.";
}
}
