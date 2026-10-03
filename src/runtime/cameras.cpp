#include "runtime/cameras.h"
#include "Game/Camera/CameraMan.h"
#include "Game/Camera/noisefilter.h"
#include "NL/nlMemory.h"
#include <cmath>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

extern cRumbleFilter* g_pRumbleFilter;
extern cNoiseFilter* g_pNoiseFilter;
namespace
{
mscharged::OriginalCameras* owner = nullptr;
std::thread::id thread;
bool busy = false, failed = false, closing = false;
cBaseCamera* deleting = nullptr;
std::set<void*> allocations;
std::set<cBaseCamera*> tracked;
void Ready(bool allow_failed = false)
{
    if (!owner || thread != std::this_thread::get_id())
        throw std::logic_error("Camera core requires its initialized owner thread");
    if (busy || closing || (!allow_failed && failed))
        throw std::logic_error("Camera core is busy or failed; callbacks may not mutate it");
}
void Unlink(cBaseCamera* camera)
{
    if (nlDLRingRemoveSafely(&cCameraManager::m_cameraStack, camera))
    {
        camera->m_next = camera->m_prev = nullptr;
        cCameraManager::m_transition = eCT_NONE;
        cCameraManager::m_pCallback = nullptr;
    }
}
}

namespace mscharged
{
void CheckNativeCameraThread()
{
    if (!owner || thread != std::this_thread::get_id())
        throw std::logic_error("Camera core requires its initialized owner thread");
}
void CheckCameraDelta(float delta)
{
    if (!std::isfinite(delta) || delta < 0)
        throw std::invalid_argument("Camera delta must be finite and nonnegative");
}
void CheckCameraVector(const nlVector3& v)
{
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
        throw std::invalid_argument("Camera vector must be finite");
}
void CheckCameraMatrix(const nlMatrix4& m)
{
    // Copy object representation: do not walk beyond the first scalar member.
    float values[16];
    static_assert(sizeof(values) == sizeof(m));
    memcpy(values, &m, sizeof(values));
    for (float v : values) if (!std::isfinite(v))
        throw std::invalid_argument("Camera matrix must be finite");
}
void CheckCameraPose(const cBaseCamera& camera)
{
    CheckCameraMatrix(camera.GetViewMatrix());
    CheckCameraVector(camera.GetCameraPosition());
    if (!std::isfinite(camera.GetFOV())) throw std::invalid_argument("Camera FOV must be finite");
}
void CheckNoiseUpdate(float elapsed, float frequency, float delta)
{
    CheckCameraDelta(delta);
    const double sample = (double(elapsed) + delta) * frequency;
    if (!std::isfinite(sample) || sample < 0 || sample > std::numeric_limits<int>::max() - 10000.0)
        throw std::out_of_range("Camera noise sample index exceeds the Wii integer range");
}
NativeCameraCall::NativeCameraCall() : exceptions_(std::uncaught_exceptions())
{
    Ready(); busy = true;
}
NativeCameraCall::~NativeCameraCall()
{
    failed = failed || std::uncaught_exceptions() > exceptions_;
    busy = false;
}
void* AllocateNativeCamera(std::size_t size, unsigned alignment, bool from_end)
{
    Ready();
    void* p = nlMalloc(size, alignment, from_end);
    try { allocations.insert(p); }
    catch (...) { nlFree(p); throw; }
    return p;
}
void FreeNativeCamera(void* p) noexcept
{
    if (!p) return;
    if (!owner || thread != std::this_thread::get_id() || !allocations.erase(p)) std::terminate();
    nlFree(p);
}
void DestroyNativeCamera(cBaseCamera* camera) noexcept
{
    if (!tracked.count(camera)) return;
    if (!owner || thread != std::this_thread::get_id() || (busy && !closing && deleting != camera)) std::terminate();
    Unlink(camera);
    tracked.erase(camera);
}
void TrackNativeCamera(cBaseCamera* camera) { tracked.insert(camera); }
void DetachNativeCamera(cBaseCamera* camera)
{
    camera->m_next = camera->m_prev = nullptr;
}
void CheckCameraInsert(cBaseCamera* camera)
{
    Ready();
    if (!camera) throw std::invalid_argument("Cannot push a null camera");
    if (cCameraManager::HasCamera(camera)) throw std::logic_error("Camera already belongs to the stack");
}
void CheckCameraPop(bool transition)
{
    Ready();
    auto* p = cCameraManager::PeekCamera();
    if (!p || (transition && p->m_next == p))
        throw std::logic_error("Camera transition requires a remaining camera");
}
void CheckCameraTransition(float duration, int transition)
{
    Ready();
    if (!cCameraManager::PeekCamera()) throw std::logic_error("Camera transition requires an existing camera");
    if (!std::isfinite(duration) || duration <= 0 || !std::isfinite(1.0f / duration) || transition != eCT_EASE_IN)
        throw std::invalid_argument("Camera transition requires a positive duration and supported mode");
}
void CheckCameraDelete(cBaseCamera* camera)
{
    if (!allocations.count(camera)) throw std::logic_error("Cannot delete a borrowed camera");
}
cRumbleFilter* CameraRumbleFilter(cBaseCamera* camera)
{
    auto* filter = dynamic_cast<cRumbleFilter*>(camera->m_pFilter[0]);
    if (!filter && camera->m_pFilter[0]) throw std::logic_error("Wrong rumble filter type");
    return filter;
}
void DeleteNativeCamera(cBaseCamera* camera)
{
    CheckCameraDelete(camera);
    const bool was_busy = busy;
    busy = true;
    deleting = camera;
    delete camera;
    deleting = nullptr;
    busy = was_busy;
}
void NotifyCameraTransition(int message)
{
    auto callback = cCameraManager::m_pCallback;
    cCameraManager::m_pCallback = nullptr;
    if (callback) callback(static_cast<eCameraMessage>(message));
}
void RemoveNativeCameraType(int type, bool destroy)
{
    Ready();
    std::vector<cBaseCamera*> found;
    auto* head = cCameraManager::m_cameraStack;
    if (!head) return;
    {
        NativeCameraCall scan;
        auto* p = head;
        do {
            if (p->GetType() == type) found.push_back(p);
            p = p->m_next;
        } while (p != head);
    }
    if (destroy) for (auto* camera : found) CheckCameraDelete(camera);
    // Original typed removal recurses through Remove; snapshot avoids its
    // changing-ring termination condition and validates all ownership first.
    for (auto* camera : found)
    {
        cCameraManager::Remove(*camera);
        if (destroy) DeleteNativeCamera(camera);
    }
}
OriginalCameras::OriginalCameras()
{
    if (owner || !gMemoryInitialized) throw std::logic_error("Camera core requires initialized, unowned game memory");
    owner = this; thread = std::this_thread::get_id(); busy = failed = closing = false;
    ResetNativeCameraState();
    try {
        g_pRumbleFilter = new (8, false) cRumbleFilter;
        g_pNoiseFilter = new (8, false) cNoiseFilter;
        live_ = true;
    } catch (...) {
        if (g_pRumbleFilter) nlDeleteGameObject(g_pRumbleFilter);
        g_pRumbleFilter = nullptr; owner = nullptr; thread = {}; throw;
    }
}
OriginalCameras::~OriginalCameras()
{
    try { Release(); } catch (...) { std::terminate(); }
}
void OriginalCameras::Release()
{
    if (!live_) return;
    Ready(true);
    closing = true;
    // The session owns allocations, including detached cameras. Borrowed objects
    // retain their lifetime and lose references to these shared session filters.
    for (auto* camera : tracked)
    {
        camera->m_next = camera->m_prev = nullptr;
        if (camera->m_pFilter[0] == g_pRumbleFilter) camera->m_pFilter[0] = nullptr;
        if (camera->m_pFilter[1] == g_pNoiseFilter) camera->m_pFilter[1] = nullptr;
    }
    cCameraManager::m_cameraStack = nullptr;
    tracked.clear();
    while (!allocations.empty()) delete static_cast<cBaseCamera*>(*allocations.begin());
    nlDeleteGameObject(g_pNoiseFilter); nlDeleteGameObject(g_pRumbleFilter);
    g_pNoiseFilter = nullptr; g_pRumbleFilter = nullptr;
    ResetNativeCameraState();
    owner = nullptr; thread = {}; live_ = busy = failed = closing = false;
}
void OriginalCameras::AttachFilters(cBaseCamera& camera)
{
    Ready();
    if (camera.m_pFilter[0] || camera.m_pFilter[1]) throw std::logic_error("Camera already has filters");
    TrackNativeCamera(&camera);
    camera.m_pFilter[0] = g_pRumbleFilter; camera.m_pFilter[1] = g_pNoiseFilter;
}
void OriginalCameras::Advance(float delta, float simulation_delta)
{
    CheckCameraDelta(delta); CheckCameraDelta(simulation_delta);
    NativeCameraCall call;
    UpdateNativeCameraPose(delta, simulation_delta);
}
cRumbleFilter& OriginalCameras::Rumble() { Ready(); return *g_pRumbleFilter; }
cNoiseFilter& OriginalCameras::Noise() { Ready(); return *g_pNoiseFilter; }
}

cBaseCamera::~cBaseCamera() { mscharged::DestroyNativeCamera(this); }
void* cBaseCamera::operator new(std::size_t size) { return mscharged::AllocateNativeCamera(size); }
void* cBaseCamera::operator new(std::size_t size, unsigned alignment, bool from_end)
{ return mscharged::AllocateNativeCamera(size, alignment, from_end); }
void cBaseCamera::operator delete(void* p) noexcept { mscharged::FreeNativeCamera(p); }
void cBaseCamera::operator delete(void* p, unsigned, bool) noexcept { mscharged::FreeNativeCamera(p); }
