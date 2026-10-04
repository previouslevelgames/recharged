#pragma once
#include "runtime/lighting.h"
#include "NL/gl/glView.h"
#include <optional>

namespace mscharged
{
// Owns the original root graph and target registry for one graphics session.
// A null drain callback permits CPU lifecycle checks; rendering requires GX.
class OriginalViews
{
    bool live_ = false;
public:
    OriginalViews(unsigned width, unsigned height, void (*drain)() = nullptr);
    ~OriginalViews();
    void Release();
    OriginalViews(const OriginalViews&) = delete;
    OriginalViews& operator=(const OriginalViews&) = delete;
};

class ViewMatrices : public GLViewInterface
{
public:
    nlMatrix4 view, projection;
    // Secondary views need their own camera for camera-relative materials.
    std::optional<nlVector3> material_camera;
    ViewMatrices() { view.SetIdentity(); projection.SetIdentity(); }
    void GetViewMatrix(nlMatrix4& out) const override { out = view; }
    void GetProjectionMatrix(nlMatrix4& out) const override { out = projection; }
    void GetInverseViewMatrix(nlMatrix4& out) const override;
    void GetViewProjectionMatrix(nlMatrix4& out) const override;
    const nlMatrix4* GetViewMatrix() const override { return &view; }
    const nlMatrix4* GetProjectionMatrix() const override { return &projection; }
};

// Child views render before their parents, using the original packet callbacks.
// Supply the active game camera in authored world units for camera overlays;
// it is independent of each child view's rendering camera.
void RenderOriginalViews(float time, const GameLighting& lighting, const nlVector3* camera_position = nullptr);
// Lifecycle-owned submission leaves packet reset to original glSendFrame.
void DispatchOriginalViews(float time, const GameLighting& lighting, const nlVector3* camera_position = nullptr);
bool OriginalViewsReady();
void SetViewFrameShutdown(void (*shutdown)());
void InitializeNativeTargets(unsigned width, unsigned height, void (*drain)());
void ShutdownNativeTargets();
// Selected game layer owners release their views before the containing graph.
void SetViewLayerShutdown(void (*shutdown)());
}
