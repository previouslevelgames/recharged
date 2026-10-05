#pragma once
#include "runtime/skin_pose.h"
#include "resources/texture_bundle.h"
#include <span>

class GLResourcePool;
class GLView;
namespace mscharged
{
enum class SkinRenderProfile
{
    // All authored hashes must resolve to real textures in the supplied RLTs.
    Authored,
    // Original non-Mario CRP_Shock diffuse replacement. Qualified Bowser shock
    // mesh only; detail uses original generated fixed-font missing binding.
    BowserShock
};
// Owns real texture registrations and retained native geometry in one marked
// pool. Input RLT bytes are decoded/copied during construction, not borrowed.
// Explicit profile selection never changes the immutable source asset.
class SkinRenderer
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    SkinRenderer(GLResourcePool&, RigidSkinAsset::Handle,
        std::span<const resources::Bytes> texture_bundles, SkinRenderProfile,
        void (*drain)());
    ~SkinRenderer();
    SkinRenderer(const SkinRenderer&)=delete;
    SkinRenderer& operator=(const SkinRenderer&)=delete;
    // One retained pose per collecting frame; errors during packet attachment
    // require send/cancel and FinishFrame before reuse. Pending matrices and
    // geometry cannot be replaced/released until the real backend drain.
    unsigned Submit(GLView&, SkinPoseFrame::Handle, int layer=0);
    void FinishFrame();
    void Release();
    bool Active() const;
    std::uint16_t TextureIndex(std::uint32_t hash) const;
};
}
