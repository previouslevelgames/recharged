#include "runtime/camera_assets.h"
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
    {
        auto sync = LoadCameraAsset(path, "fechoosecaptains");
        CameraAssetLoad request(path, "fechoosecaptains");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!request.Ready())
        {
            request.Service();
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Camera asset read timed out");
            if (!request.Ready()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto async = request.Result();
        const auto& a = sync->Data(); const auto& b = async->Data(); count = a.m_uKeyCount;
        if (count != b.m_uKeyCount || a.m_uHashID != b.m_uHashID || std::strcmp(a.field_0x0C, b.field_0x0C)
            || std::memcmp(a.cameraPos,b.cameraPos,count*sizeof(nlVector3))
            || std::memcmp(a.targetPos,b.targetPos,count*sizeof(nlVector3))
            || std::memcmp(a.cameraRot,b.cameraRot,count*sizeof(nlQuaternion))
            || std::memcmp(a.fFOV,b.fFOV,count*sizeof(float))
            || std::memcmp(a.fFocalLength,b.fFocalLength,count*sizeof(float)))
            throw std::runtime_error("Sync/async native camera records differ");
        CameraAssetLibrary library;
        library.Insert(async);
        auto selected = library.Find("FECHOOSECAPTAINS");
        library.Clear();
        if (selected != async || selected->Data().m_uKeyCount != count)
            throw std::runtime_error("Camera asset handle lifetime differs");
    }
    if (StandardAllocator.TotalFreeMemory() != mem1 || VirtualAllocator.TotalFreeMemory() != mem2)
        throw std::runtime_error("Camera asset decoding did not recover both arenas");
    return "Native camera asset decoded through sync/async NL reads: " + std::string(path)
        + " (" + std::to_string(count) + " keys); both arenas recovered. Original animated-camera playback remains pending.";
}
}
