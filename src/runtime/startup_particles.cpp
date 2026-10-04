#include "runtime/startup.h"
#include "runtime/particle_files.h"
#include "resources/effects_bundle.h"
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
    const auto bundle = resources::ReadEffectsBundle((*files)[ParticleFileKind::Resident]);
    const auto textures = resources::ReadEffectsTextureBundle((*files)[ParticleFileKind::NonResident]);
    const auto geometry_textures = resources::ReadTextureBundle((*files)[ParticleFileKind::Textures]);
    std::size_t templates = 0, groups = 0;
    for (const auto& entry : bundle.entries)
    { templates += entry.templates.size(); groups += entry.groups.size(); }
    return "Independent particle resource diagnostic: 4 files, " + std::to_string(templates)
        + " templates, " + std::to_string(groups) + " groups, "
        + std::to_string(textures.textures.size()) + " effects textures and "
        + std::to_string(geometry_textures.textures.size()) + " geometry textures decoded; "
        + std::to_string((*files)[ParticleFileKind::Geometry].size())
        + " geometry bytes retained. Original bundle/geometry registration and particle rendering remain unavailable.";
}
}
