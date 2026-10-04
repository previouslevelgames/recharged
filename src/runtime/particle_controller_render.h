#pragma once
#include "runtime/particle_controller.h"
class GLResourcePool;
class GLView;
namespace mscharged
{
// One marked pool scope registers each authored texture once for every retained
// controller/system. Must outlive collecting frames and be released before the
// controllers, pool, or graphics context. Start/Destroy/Reset reject this lease.
class ParticleControllerRenderer
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    ParticleControllerRenderer(GLResourcePool&, ParticleControllers&, void (*drain)());
    ~ParticleControllerRenderer();
    ParticleControllerRenderer(const ParticleControllerRenderer&) = delete;
    ParticleControllerRenderer& operator=(const ParticleControllerRenderer&) = delete;
    unsigned Submit(GLView&, bool visible = true, float aspect = 1, bool allow_in_front = true);
    void FinishFrame();
    void Release();
    bool Active() const;
    std::size_t Textures() const;
};
}
