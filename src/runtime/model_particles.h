#pragma once
#include "runtime/animation_pose.h"
#include "runtime/effects_vertex.h"
#include "runtime/particle_simulation.h"
#include <optional>

class GLView;
namespace mscharged
{
struct ModelParticleGround
{
    bool enabled; // Explicit original manager ground-projection policy.
    float height, shadow_height;
};
struct ModelParticleInputs
{
    std::array<float,3> position{0,0,0}, direction{0,0,1}, velocity{0,0,0};
    std::uint16_t facing = 0;
    bool visible = true;
    AnimationPoseFrame::Handle pose;
    bool mirror = false;
    std::uint32_t joint_override = 0;
    std::optional<ModelParticleGround> ground;
};
struct ModelParticleSample
{
    nlMatrix4 matrix;
    std::uint32_t frame;
    std::array<std::uint8_t,4> colour;
};

// One retained original animated-model effect spec. Explicit pose/ground inputs
// supply the selected attachment contracts; no gameplay actor or NIS readiness
// is implied. The original atlas, particle pool and RNG domain remain exclusive.
// Nonanimated models, absent textures/models, ascending joints, terrain, user
// effects and lights remain unsupported. Rest-pose inputs are diagnostic only.
class ModelParticles
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    ModelParticles(EffectsRegistry::Handle, std::shared_ptr<EffectsVertexResources>,
        std::uint32_t group, std::size_t spec, ModelParticleInputs,
        ParticleSimulationOptions = {});
    ~ModelParticles();
    ModelParticles(const ModelParticles&) = delete;
    ModelParticles& operator=(const ModelParticles&) = delete;
    // Validate complete replacement inputs before mutating the original system.
    // Pose matrices are immutable retained snapshots, never live actor pointers.
    void SetInputs(ModelParticleInputs);
    bool Advance(float delta);
    std::vector<ParticleSnapshot> Snapshot() const;
    std::vector<ModelParticleSample> Sample();
    // Original model-particle frame/matrix/material/raster/layer operations.
    // Finish after either sending or cancelling, including failed submission.
    unsigned Submit(GLView&);
    void FinishFrame();
    void Die();
    void Reset(std::uint32_t seed);
    void Release();
    bool Active() const;
    bool Failed() const;
    std::uint32_t Seed() const;
};
}
