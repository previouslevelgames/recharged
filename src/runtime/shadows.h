#pragma once
#include "Game/Render/RLViewLayers.h"
#include "Game/Render/RenderShadow.h"
#include "NL/gl/glMemory.h"
#include <cstdint>

namespace mscharged
{
// Four selected original layers and their eleven projection targets. The caller
// supplies real view matrices and geometry; game character/scene selection is pending.
class ShadowLayers
{
    bool live_ = false;
public:
    explicit ShadowLayers(GLViewInterface& scene_camera);
    ~ShadowLayers();
    void Release();
    RLView& Layer(eCLV layer) const;
    GLView& Partition(unsigned index) const;
    unsigned PartitionTexture(unsigned index) const;
    void PreparePartition(const ProjectedShadowParams& params) const;
    bool ShouldUpdate(const ProjectedShadowParams& params, const GLViewInterface& visible_camera,
                      unsigned frame) const;
    void ResetPartitions() const;
    ShadowLayers(const ShadowLayers&) = delete;
    ShadowLayers& operator=(const ShadowLayers&) = delete;
};

// Selected original StadiumShadowVolumeDrawable behavior. Owns only cloned
// packets/material parameters; the source inventory owns shared geometry and
// must outlive this object. Release submitted views before releasing resources.
class StadiumShadowVolume
{
    GLResourcePool* pool_ = nullptr;
    glModel* models_[2]{};
    std::uint64_t submitted_frame_ = 0;
public:
    explicit StadiumShadowVolume(glModel& source);
    ~StadiumShadowVolume();
    void Draw(const nlMatrix4& world);
    void Release();
    StadiumShadowVolume(const StadiumShadowVolume&) = delete;
    StadiumShadowVolume& operator=(const StadiumShadowVolume&) = delete;
};
}
