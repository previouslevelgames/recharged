#include "runtime/shadows.h"
#include "runtime/views.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glMatrix.h"
#include "NL/glx/GXShadowVolumeMaterialProgram.h"
#include "NL/glx/glxGX.h"
#include <dolphin/gx.h>
#include <stdexcept>

void SetupNativeShadowLayers(GLViewInterface*);
void ShutdownNativeShadowLayers();
GLView* GetShadowPartitionView(int);
u32 GetShadowPartitionTexture(int);
void PrepareNativeShadowPartition(const ProjectedShadowParams&);
bool ShouldShadowBeUpdated(const ProjectedShadowParams&, const GLViewInterface&, u32);
void InitializeNativeStadiumShadowModels(glModel*, glModel**, GLResourcePool*);
void DrawNativeStadiumShadowModels(glModel* const*);

// The selected original procedural writer uses frame arrays. Persistent Wii
// display-list generation remains an explicit unsupported path.
void glplatFinalizePacket(glModelPacket* packet, bool permanent, void*)
{
    if (!packet || !packet->materialProgram || permanent)
        throw std::invalid_argument("Native procedural packet requires frame storage");
    static_cast<GLMaterialProgram*>(packet->materialProgram)->Configure(packet);
    packet->displayList = nullptr;
    packet->skinnedNormals = packet->skinnedVertices = 0;
}
void glx_Fog(bool enabled)
{
    if (enabled) throw std::runtime_error("Stadium fog environment is not connected");
    GXSetFog(GX_FOG_NONE, 0, 1, 0, 1, {0, 0, 0, 0});
}
bool gxSetCoPlanar(bool enabled)
{
    static bool previous = false;
    GXSetCoPlanar(enabled);
    const bool old = previous; previous = enabled; return old;
}
namespace mscharged
{
namespace { ShadowLayers* active = nullptr; }
ShadowLayers::ShadowLayers(GLViewInterface& camera)
{
    if (active) throw std::logic_error("Shadow layers already active");
    SetupNativeShadowLayers(&camera);
    try { SetViewLayerShutdown([] { active->Release(); }); }
    catch (...) { ShutdownNativeShadowLayers(); throw; }
    active = this;
    live_ = true;
}
ShadowLayers::~ShadowLayers() { Release(); }
void ShadowLayers::Release()
{
    if (!live_) return;
    if (glNativeViewDispatchActive()) throw std::logic_error("Cannot release shadows during dispatch");
    ShutdownNativeShadowLayers();
    SetViewLayerShutdown(nullptr);
    active = nullptr;
    live_ = false;
}
RLView& ShadowLayers::Layer(eCLV layer) const
{
    if (!live_) throw std::logic_error("Shadow layers are inactive");
    return *GetLayerView(layer);
}
GLView& ShadowLayers::Partition(unsigned index) const
{
    if (!live_ || index >= 11) throw std::out_of_range("Inactive or invalid shadow partition");
    return *GetShadowPartitionView(int(index));
}
unsigned ShadowLayers::PartitionTexture(unsigned index) const
{
    Partition(index);
    return GetShadowPartitionTexture(int(index));
}
void ShadowLayers::PreparePartition(const ProjectedShadowParams& params) const
{
    Partition(params.nPartitionIndex);
    PrepareNativeShadowPartition(params);
}
bool ShadowLayers::ShouldUpdate(const ProjectedShadowParams& params, const GLViewInterface& camera, unsigned frame) const
{
    Partition(params.nPartitionIndex);
    return ShouldShadowBeUpdated(params, camera, frame);
}
void ShadowLayers::ResetPartitions() const
{
    if (!live_) throw std::logic_error("Shadow layers are inactive");
    ClearCharacterShadowsUpdated();
}

StadiumShadowVolume::StadiumShadowVolume(glModel& source)
{
    if (!source.packets || !source.numPackets || source.numPackets > 4096)
        throw std::invalid_argument("Shadow drawable requires static model packets");
    auto* program = glGetMaterialProgram(0x386ecbdd);
    if (!program) throw std::logic_error("Shadow drawable requires its original material");
    for (unsigned i = 0; i < source.numPackets; ++i)
    {
        const auto& packet = source.packets[i];
        if (packet.materialProgram != program || !packet.materialParameters || !packet.streams
            || packet.numStreams != 3 || !packet.indexBuffer || !packet.numVertices
            || static_cast<const GXShadowVolumeParameters*>(packet.materialParameters)->useFixedColour != 1)
            throw std::invalid_argument("Shadow drawable needs indexed fixed-colour volume packets");
    }
    // Each original allocation is rounded to 32 bytes by the resource pool.
    const GLMemoryRequirement requirement{GLM_Header,
        256 + 2ul * source.numPackets * (sizeof(glModelPacket) + 64)};
    pool_ = glCreateResourcePool(&requirement, 1, "Stadium shadow packets");
    try { InitializeNativeStadiumShadowModels(&source, models_, pool_); }
    catch (...) { glDestroyResourcePool(pool_); pool_ = nullptr; throw; }
}
StadiumShadowVolume::~StadiumShadowVolume() { Release(); }
void StadiumShadowVolume::Release()
{
    if (!pool_) return;
    if (glNativeViewDispatchActive()) throw std::logic_error("Cannot release shadow drawable during dispatch");
    glDestroyResourcePool(pool_); pool_ = nullptr;
    models_[0] = models_[1] = nullptr;
}
void StadiumShadowVolume::Draw(const nlMatrix4& world)
{
    if (!pool_ || !active) throw std::logic_error("Shadow drawable requires live resources and layers");
    const auto frame = glNativeFrameGeneration();
    if (submitted_frame_ == frame) throw std::logic_error("Shadow drawable already submitted this frame");
    glModelSetMatrix(models_[0], world);
    glModelSetMatrix(models_[1], models_[0]->packets[0].matrix);
    DrawNativeStadiumShadowModels(models_);
    submitted_frame_ = frame;
}
}
