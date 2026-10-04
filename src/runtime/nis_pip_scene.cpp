#include "runtime/nis_pip_scene.h"
#include "NL/gl/glMatrix.h"
#include <dolphin/gx/GXAurora.h>
#include <stdexcept>

namespace mscharged
{
namespace
{
constexpr unsigned quad_id = 1;
void Drain() { AuroraGXSync(); }
}
NisPipScene::NisPipScene()
{
    const GLMemoryRequirement requirements[] = {{GLM_Header, 65536}, {GLM_VertexData, 4096}};
    pool_.reset(glCreateResourcePool(requirements, 2, "NIS PIP rectangle"));
    resources::Packet packet;
    packet.material.program = 0x21db4385; // Original unlit diffuse polygon.
    packet.material.textures[0] = {static_cast<std::uint32_t>(target_.Texture()), 3};
    packet.raster = 0xc0000; // RGBA writes, no depth test/write or culling.
    packet.vertices = {{{0,0,0},{0,0}}, {{1,0,0},{1,0}}, {{1,1,0},{1,1}}, {{0,1,0},{0,1}}};
    packet.indices = {0,1,2,0,2,3};
    quad_ = std::make_unique<StaticInventory>(*pool_, std::vector<resources::StaticModel>{{quad_id,{packet}}},
        std::vector<resources::Texture>{}, Drain);
    glMatrixOrthographic(overlay_.projection, 640, 480);
    auto opaque = std::make_unique<GLView>(&secondary_, GLRenderPair{}, GLViewSort_Texture);
    opaque->m_Name = "NIS secondary opaque";
    opaque->m_ClearColour = opaque->m_ClearDepth = true;
    opaque->SetViewport(0,0,target_.ViewportWidth(),target_.ViewportHeight());
    alpha_ = std::make_unique<GLView>(&secondary_, target_.Pair(), GLViewSort_Texture);
    alpha_->m_Name = "NIS secondary alpha/copy";
    alpha_->m_Target = GLViewTarget_Mode9;
    alpha_->SetViewport(0,0,target_.ViewportWidth(),target_.ViewportHeight());
    alpha_->AddChild(opaque.get()); opaque_ = opaque.release();
    overlay_view_ = std::make_unique<GLView>(&overlay_, GLRenderPair{}, GLViewSort_None);
    overlay_view_->m_Name = "NIS picture in picture";
    gRootView.AddChild(alpha_.get());
}
NisPipScene::~NisPipScene()
{
    Drain();
    overlay_view_.reset(); alpha_.reset(); // Detach before releasing texture/model memory.
    quad_.reset(); pool_.reset();
}
void NisPipScene::AttachOverlay() { gRootView.AddChild(overlay_view_.get()); }
void NisPipScene::SetCamera(const cBaseCamera& camera)
{
    CheckCameraPose(camera);
    secondary_.view = camera.GetViewMatrix();
    secondary_.material_camera = camera.GetCameraPosition();
    // RLViewCamera uses the original 4:3 projection for the stretched PIP copy.
    glMatrixPerspective(secondary_.projection, camera.GetFOV() * 3.1415927f / 180, 4.f/3, .25f, 4096.f);
}
void NisPipScene::Submit(const NisPip& pip)
{
    const auto rect = pip.Rectangle();
    if (!rect) return;
    nlMatrix4 matrix; matrix.SetIdentity();
    matrix.m11 = rect->width; matrix.m22 = rect->height;
    matrix.m41 = rect->x; matrix.m42 = rect->y;
    auto* model = quad_->Model(quad_id);
    glModelSetMatrix(model, matrix);
    overlay_view_->AttachModel(model, 0);
}
}
