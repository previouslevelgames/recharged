#pragma once
#include "runtime/nis_pip.h"
#include "runtime/nis_pip_render.h"
#include "runtime/static_inventory.h"
#include "runtime/views.h"
#include <memory>

namespace mscharged
{
// Secondary opaque/alpha view pair and the original PIP textured rectangle.
// Construct before the primary views, then AttachOverlay after them. Release
// after frame completion and before the containing OriginalViews session.
class NisPipScene
{
    NisPipTarget target_;
    ViewMatrices secondary_, overlay_;
    std::unique_ptr<GLResourcePool, void (*)(GLResourcePool*)> pool_{nullptr, glDestroyResourcePool};
    std::unique_ptr<StaticInventory> quad_;
    std::unique_ptr<GLView> alpha_, overlay_view_;
    GLView* opaque_ = nullptr;
public:
    NisPipScene();
    ~NisPipScene();
    NisPipScene(const NisPipScene&) = delete;
    NisPipScene& operator=(const NisPipScene&) = delete;
    void AttachOverlay();
    void SetCamera(const cBaseCamera& camera);
    void Submit(const NisPip& pip);
    GLView& Opaque() { return *opaque_; }
    GLView& Alpha() { return *alpha_; }
    const ViewMatrices& Matrices() const { return secondary_; }
};
}
