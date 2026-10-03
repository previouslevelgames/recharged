#include "runtime/cameras.h"
#include "runtime/tasks.h"
#include "Game/Camera/CameraMan.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlTask.h"
#include "NL/gl/glMatrix.h"
#include <stdexcept>

namespace mscharged
{
std::string VerifyStartupCameraCore()
{
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    struct RestoreSeed { unsigned value = nlDefaultSeed; ~RestoreSeed() { nlDefaultSeed = value; } } seed;
    nlTaskManager::Startup(4);
    try
    {
        OriginalCameras cameras;
        CameraPoseInput first, second;
        first.position = {0,0,4}; second.position = {2,0,4};
        first.fov = 40; second.fov = 60;
        glMatrixLookAt(first.view, first.position, first.target, {0,1,0});
        glMatrixLookAt(second.view, second.position, second.target, {0,1,0});
        cameras.AttachFilters(first); cameras.AttachFilters(second);
        cCameraManager::PushCamera(&first);
        cameras.Advance(0,0);
        cCameraManager::PushCameraWithTransition(&second, 1, eCT_EASE_IN, nullptr, false);
        cameras.Advance(0.5f,0.5f);
        cameras.Advance(0,0);
        if (cCameraManager::m_fFOV != 50) throw std::runtime_error("Original camera midpoint interpolation failed");
        FireCameraRumbleFilter(0.01f,0.02f,5000,10);
        nlVector3 amplitude{0.01f,0.01f,0.01f};
        FireCameraNoiseFilter(amplitude,10,1);
        cameras.Advance(0.01f,0.01f);
        if (cCameraManager::PopCamera() != &second || cCameraManager::PopCamera() != &first)
            throw std::runtime_error("Original camera stack order failed");
    }
    catch (...) { ShutdownNativeTaskManager(); throw; }
    ShutdownNativeTaskManager();
    if (StandardAllocator.TotalFreeMemory() != mem1 || VirtualAllocator.TotalFreeMemory() != mem2)
        throw std::runtime_error("Camera core did not recover both game arenas");
    return "Original camera stack, transition and filters verified with supplied diagnostic poses; both arenas recovered. Authored cameras remain pending.";
}
}
