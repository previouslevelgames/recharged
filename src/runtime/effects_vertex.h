#pragma once
#include "resources/static_model.h"
#include "resources/texture_bundle.h"
#include "NL/gl/glModel.h"
#include <functional>
#include <memory>

class GLResourcePool;
namespace mscharged
{
enum class EffectsVertexMode { Loop, Hold };
struct EffectsVertexState
{
    std::uint32_t frames = 0;
    float frame = 0, speed = 1;
    EffectsVertexMode mode = EffectsVertexMode::Loop;
    bool done = false;
};
struct EffectsVertexMemory
{
    std::size_t headers = 2 * 1024 * 1024;
    std::size_t geometry = 4 * 1024 * 1024;
    std::size_t textures = 8 * 1024 * 1024;
};
// Dedicated original GL inventory and pool, retaining decoded effects models,
// textures and source position-animation arrays until all frame users drain.
// Requires live graphics memory/material programs; does not initialize the
// effects/particle manager or provide skin/morph/user-effect services.
class EffectsVertexResources
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    EffectsVertexResources(const resources::EffectsGeometry&, const resources::TextureBundle&,
        std::function<void()> drain, EffectsVertexMemory = {});
    // Register into the caller's exact live pool under a retained marker. The
    // caller must keep that pool and its preceding resources alive until Release;
    // no pool destruction or global inventory substitution occurs in this mode.
    EffectsVertexResources(GLResourcePool&, const resources::EffectsGeometry&,
        const resources::TextureBundle&, std::function<void()> drain);
    const GLResourcePool* Pool() const;
    ~EffectsVertexResources();
    EffectsVertexResources(const EffectsVertexResources&) = delete;
    EffectsVertexResources& operator=(const EffectsVertexResources&) = delete;
    bool Active() const;
    std::size_t Size() const;
    EffectsVertexState State(std::uint32_t model) const;
    void Configure(std::uint32_t model, EffectsVertexMode, float speed = 1);
    void Reset(std::uint32_t model);
    // Calls each owned original GLVertexAnim::Update once. Original inventory's
    // aggregate repeats the top level; that multi-level scheduler is unselected.
    void Update(float delta);
    // Current negative sentinel or explicit checked frame, in a collecting GL
    // frame. Multiple clones keep independent stream descriptors; vertices,
    // indices/material bindings remain retained until FinishFrame drains.
    glModel* Model(std::uint32_t model, int frame = -1);
    void FinishFrame();
    void Release();
};
}
