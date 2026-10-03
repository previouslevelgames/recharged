#pragma once
#include "Game/Camera/BaseCam.h"
#include <cstddef>
#include <string>

class cBaseCamera;
class cRumbleFilter;
class cNoiseFilter;
class nlVector3;
class nlMatrix4;
namespace mscharged
{
// Owns the selected camera core and native camera allocations for one session.
// Stack cameras are borrowed. Popped allocations remain owned until deletion or
// Release. Full camera factories, assets and gameplay Update are separate.
class OriginalCameras
{
public:
    OriginalCameras();
    ~OriginalCameras();
    OriginalCameras(const OriginalCameras&) = delete;
    OriginalCameras& operator=(const OriginalCameras&) = delete;
    void Release();
    void AttachFilters(cBaseCamera& camera);
    void Advance(float delta, float simulation_delta);
    cRumbleFilter& Rumble();
    cNoiseFilter& Noise();
private:
    bool live_ = false;
};

void* AllocateNativeCamera(std::size_t size, unsigned alignment = 8, bool from_end = false);
void FreeNativeCamera(void* pointer) noexcept;
void DestroyNativeCamera(cBaseCamera* camera) noexcept;
void CheckNativeCameraThread();
void CheckCameraInsert(cBaseCamera* camera);
void CheckCameraPop(bool transition);
void CheckCameraTransition(float duration, int transition);
void CheckCameraDelete(cBaseCamera* camera);
cRumbleFilter* CameraRumbleFilter(cBaseCamera* camera);
void DeleteNativeCamera(cBaseCamera* camera);
void TrackNativeCamera(cBaseCamera* camera);
void DetachNativeCamera(cBaseCamera* camera);
void RemoveNativeCameraType(int type, bool destroy);
void NotifyCameraTransition(int message);
void ResetNativeCameraState();
void UpdateNativeCameraPose(float delta, float simulation_delta);
void CheckCameraDelta(float delta);
void CheckCameraPose(const cBaseCamera& camera);
void CheckCameraMatrix(const nlMatrix4& matrix);
void CheckCameraVector(const nlVector3& vector);
void CheckNoiseUpdate(float elapsed, float frequency, float delta);

class NativeCameraCall
{
    int exceptions_;
public:
    NativeCameraCall();
    ~NativeCameraCall();
    NativeCameraCall(const NativeCameraCall&) = delete;
    NativeCameraCall& operator=(const NativeCameraCall&) = delete;
};
std::string VerifyStartupCameraCore();

// Explicit pose input for diagnostic callers. Original authored/gameplay camera
// classes remain separate; this camera does not load assets or choose a scene.
class CameraPoseInput final : public cBaseCamera
{
public:
    nlMatrix4 view;
    nlVector3 position{}, target{};
    float fov = 40;
    CameraPoseInput() { view.SetIdentity(); }
    eCameraType GetType() override { return eCameraType_Debug; }
    void Update(float delta) override { CheckCameraDelta(delta); }
    const nlMatrix4& GetViewMatrix() const override { return view; }
    const nlVector3& GetCameraPosition() const override { return position; }
    const nlVector3& GetTargetPosition() const override { return target; }
    float GetFOV() const override { return fov; }
};
}
