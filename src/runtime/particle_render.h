#pragma once
#include "runtime/particle_simulation.h"
#include <cstdint>
#include <memory>
class GLResourcePool;
class GLView;
namespace mscharged
{
// Registers the simulation's actual retained texture in a marked native pool.
// The pool, simulation and initialized graphics session outlive this owner.
// Submit runs during the original collecting frame, before its view dispatch.
// FinishFrame follows original send/cancel and GPU drain; Release rejects any
// pending frame before rewinding the pool. No model/light/user-effect service.
class ParticleRenderer
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    ParticleRenderer(GLResourcePool& pool, ParticleSimulation& simulation, void (*drain)());
    ~ParticleRenderer();
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;
    // Preserves original per-particle list order and authored GL layer. The
    // explicit visibility/aspect are the original renderer's diagnostic inputs.
    // allow_in_front defaults to original m_AllowInFront=true independently of
    // the template/spec's authored in-front bit.
    unsigned Submit(GLView& view, bool visible = true, float aspect = 1, bool allow_in_front = true);
    void FinishFrame();
    void Release();
    bool Active() const;
    std::uint16_t TextureIndex() const;
};
}
