#include "runtime/frontend_camera_assets.h"
#include "runtime/animated_camera.h"
#include "Game/Camera/CameraMan.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace mscharged
{
std::string VerifyStartupCameraAssets()
{
    constexpr auto path = "/Art/fe/environments/cameras/camera_idle.cam";
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    unsigned long count = 0;
    std::size_t requested = 0, completed = 0;
    {
        auto sync = LoadCameraAsset(path, "fechoosecaptains");
        CameraAssetLibrary library;
        auto request = LoadFrontendCameraAssets(library);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (request.State() == CameraBatchState::Loading)
        {
            request.Service();
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Camera asset read timed out");
            if (request.State() == CameraBatchState::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        request.Publish();
        const auto progress = request.Progress();
        requested = progress.requested; completed = progress.completed;
        if (requested != FrontendCameraCatalog().size() || completed != requested
            || progress.succeeded != requested || progress.failed || progress.cancelled || progress.pending || progress.active)
            throw std::runtime_error("Frontend camera catalog accounting differs");
        auto async = library.Find("fechoosecaptains");
        if (!async) throw std::runtime_error("Original frontend idle alias is absent after publication");
        const auto& a = sync->Data(); const auto& b = async->Data(); count = a.m_uKeyCount;
        if (count != b.m_uKeyCount || a.m_uHashID != b.m_uHashID || std::strcmp(a.m_szName, b.m_szName)
            || std::memcmp(a.cameraPos,b.cameraPos,count*sizeof(nlVector3))
            || std::memcmp(a.targetPos,b.targetPos,count*sizeof(nlVector3))
            || std::memcmp(a.cameraRot,b.cameraRot,count*sizeof(nlQuaternion))
            || std::memcmp(a.fFOV,b.fFOV,count*sizeof(float))
            || std::memcmp(a.fFocalLength,b.fFocalLength,count*sizeof(float)))
            throw std::runtime_error("Sync/async native camera records differ");
        auto selected = library.Find("FECHOOSECAPTAINS");
        library.Clear();
        if (selected != async || selected->Data().m_uKeyCount != count)
            throw std::runtime_error("Camera asset handle lifetime differs");
        OriginalCameras cameras;
        AnimatedCamera playback(selected);
        cCameraManager::PushCamera(&playback.Camera());
        for (float time : {0.f, .25f, .5f, .75f, 1.f})
        {
            playback.Seek(time);
            cameras.Advance(0, 0);
            CheckCameraPose(playback.Camera());
        }
    }
    if (StandardAllocator.TotalFreeMemory() != mem1 || VirtualAllocator.TotalFreeMemory() != mem2)
        throw std::runtime_error("Camera asset decoding did not recover both arenas");
    return "Native camera asset decoded through sync/async NL reads: " + std::string(path)
        + " (" + std::to_string(count) + " keys); original authored playback sampled through CameraMan; both arenas recovered."
        + " Original frontend camera catalog: " + std::to_string(requested) + " requested, "
        + std::to_string(completed) + " completed, " + std::to_string(completed) + " published; named assets only, not the full camera factory.";
}
}
