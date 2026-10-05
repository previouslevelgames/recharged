#pragma once
#include "runtime/camera_assets.h"
#include "runtime/cameras.h"
#include "Game/Camera/CameraMan.h"
#include <string>
#include <functional>
#include <thread>
#include <vector>

namespace mscharged
{
inline constexpr std::size_t MaximumFrontendCameras = 16;
struct FrontendCameraSelection;
using FrontendCameraSelectionHandle = std::shared_ptr<const FrontendCameraSelection>;

// Bounded ownership for the original FE presentation-camera operations. The
// core, library and native arenas must outlive this owner. Only original catalog
// aliases are accepted; no FE world/state or global animation registry is built.
// The stack may have a borrowed base camera, which this owner never deletes.
class FrontendCameras
{
    class Camera;
    class Operation;
    OriginalCameras& core_;
    const CameraAssetLibrary& library_;
    std::thread::id thread_ = std::this_thread::get_id();
    std::vector<Camera*> cameras_;
    static thread_local FrontendCameras* transition_owner_;
    void (*callback_)(eCameraMessage) = nullptr;
    bool busy_ = false, failed_ = false, released_ = false;
    void CheckThread() const;
    void Ready() const;
    Camera* Owned(cBaseCamera* camera) const;
    void Forget(Camera* camera) noexcept;
    static void TransitionCallback(eCameraMessage message);
    void OwnTransition(void (*callback)(eCameraMessage)) noexcept;
    void CancelTransition(bool live_core) noexcept;
    void Destroy(bool live_core) noexcept;
    Camera& Selected(const FrontendCameraSelectionHandle&, bool active = true) const;
public:
    FrontendCameras(OriginalCameras& core, const CameraAssetLibrary& library);
    ~FrontendCameras();
    FrontendCameras(const FrontendCameras&) = delete;
    FrontendCameras& operator=(const FrontendCameras&) = delete;

    // Original FE semantics: speed 1, cyclic authored playback, ease-in for
    // positive duration. Zero-duration changes do not invoke the new callback.
    // Returned identities remain valid only until that camera is popped,
    // replaced or released. Validation errors preserve the active stack;
    // failures after preparation begins require Release before further use.
    const cBaseCamera& Push(const std::string& alias, void (*callback)(eCameraMessage) = nullptr,
        float duration = 0, bool delete_current = false);
    void Pop(void (*callback)(eCameraMessage) = nullptr, float duration = 0);
    void Advance(float delta, float simulation_delta);
    const cBaseCamera* ActiveCamera() const;
    std::string ActiveAlias() const;
    // Exact retained selection identity; reselection preserves the CameraMan
    // wrapper and old time/view but invalidates prior selection handles. Current
    // selection requires an owned top camera with no active manager transition.
    FrontendCameraSelectionHandle Selection() const;
    FrontendCameraSelectionHandle Select(const FrontendCameraSelectionHandle& expected,
        const std::string& alias, bool cyclic, std::function<void()> on_end = {});
    void Seek(const FrontendCameraSelectionHandle&, float normalized_time);
    void SetCyclic(const FrontendCameraSelectionHandle&, bool);
    float Time(const FrontendCameraSelectionHandle&) const;
    float Duration(const FrontendCameraSelectionHandle&) const;
    // Remove only this exact selection's callback, including an inactive owned
    // camera. Allowed after failure/core release; never removes a later binding.
    bool DetachEndCallback(const FrontendCameraSelectionHandle&);
    std::size_t Size() const;
    bool Failed() const;

    // Teardown cancels transitions without user callbacks, like the native
    // camera session owner, including a pop onto a borrowed base. Safe after an
    // operation/callback failure. Destroy
    // before the core or call core.Release() while this owner remains alive.
    void Release();
};
}
