#include "runtime/frames.h"
#include "runtime/views.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glPlat.h"
#include "NL/gl/glState.h"
#include "NL/glx/glxMemory.h"
#include <exception>
#include <stdexcept>

namespace
{
mscharged::OriginalFrames* active = nullptr;
mscharged::OriginalFrames& Frames()
{
    if (!active) throw std::logic_error("Original graphics frames are not initialized");
    return *active;
}
}
void glplatBeginFrame() { Frames().Begin(); }
void glplatEndFrame() { Frames().End(); }
void glplatSendFrame() { Frames().Submit(true); }
void glplatAbortFrame() { Frames().Submit(false); }
void glplatFinish() { Frames().Drain(); }

namespace mscharged
{
OriginalFrames::OriginalFrames(FrameBackend& backend) : backend_(backend), thread_(std::this_thread::get_id())
{
    if (active || !OriginalViewsReady() || !glGetCurrentResourcePool())
        throw std::logic_error("Original frames require an exclusive initialized view/memory session");
    SetViewFrameShutdown([] { Frames().Release(); });
    ResetOriginalFrameState();
    active = this;
    live_ = true;
}
OriginalFrames::~OriginalFrames() { Release(); }
void OriginalFrames::CheckThread() const
{
    if (!live_ || thread_ != std::this_thread::get_id())
        throw std::logic_error("Graphics frames require their initialized owner thread");
}
bool OriginalFrames::Acquire()
{
    CheckThread();
    if (busy_ || phase_ != Phase::Idle) throw std::logic_error("A host frame is already in progress");
    busy_ = true;
    try
    {
        const bool acquired = backend_.Acquire();
        busy_ = false;
        if (acquired)
        {
            generation_ = glNativeFrameGeneration();
            phase_ = Phase::Acquired;
        }
        return acquired;
    }
    catch (...) { backend_.Cancel(); busy_ = false; throw; }
}
void OriginalFrames::CheckPlatformCall(FrameCall call)
{
    CheckThread();
    if (!busy_ || call_ != call || platform_called_)
        throw std::logic_error("Platform frame service must be called once through its original entry point");
    platform_called_ = true;
}
void OriginalFrames::Begin() { CheckPlatformCall(FrameCall::Begin); phase_ = Phase::Collecting; }
void OriginalFrames::End() { CheckPlatformCall(FrameCall::End); phase_ = Phase::Ended; }
void OriginalFrames::Submit(bool present)
{
    CheckPlatformCall(FrameCall::Send);
    if (present) backend_.Render();
    backend_.Drain();
    // Packet dispatch can select a matrix in the retiring frame. Never retain
    // that borrowed pointer as current state after its storage is recycled.
    glSetCurrentMatrix(glGetIdentityMatrix());
    glplatFrameAllocNextFrame();
    backend_.Finish(present);
    backend_.Drain();
    // The original glSendFrame resets views and commits its state/counter next.
    phase_ = Phase::Submitted;
}
void OriginalFrames::Drain() { CheckPlatformCall(FrameCall::Finish); backend_.WaitIdle(); }
void OriginalFrames::Recover() noexcept
{
    backend_.Cancel(); // Includes a drain even if Finish already closed the frame.
    try
    {
        gl_ViewReset();
        glSetCurrentMatrix(glGetIdentityMatrix());
        if (glNativeFrameGeneration() == generation_) glplatFrameAllocNextFrame();
        backend_.Drain();
        CancelOriginalFrameState();
        phase_ = Phase::Idle;
    }
    catch (...) { std::terminate(); } // Never continue with stale frame pointers.
}
void OriginalFrames::Cancel()
{
    CheckThread();
    if (busy_) throw std::logic_error("Cannot cancel graphics during a frame callback");
    if (phase_ != Phase::Idle) Recover();
}
void OriginalFrames::Release()
{
    if (!live_) return;
    Cancel();
    backend_.Drain();
    SetViewFrameShutdown(nullptr);
    ResetOriginalFrameState();
    active = nullptr;
    live_ = false;
}
NativeFrameGuard::NativeFrameGuard(FrameCall call)
    : owner_(&Frames()), exceptions_(std::uncaught_exceptions())
{
    owner_->CheckThread();
    if (owner_->busy_) throw std::logic_error("Recursive original graphics frame operation");
    const auto phase = owner_->phase_;
    if ((call == FrameCall::Begin && phase != OriginalFrames::Phase::Acquired)
        || (call == FrameCall::End && phase != OriginalFrames::Phase::Collecting)
        || (call == FrameCall::Send && phase != OriginalFrames::Phase::Ended)
        || (call == FrameCall::Finish && phase != OriginalFrames::Phase::Idle))
        throw std::logic_error("Original graphics frame operation is out of order");
    owner_->busy_ = true;
    owner_->call_ = call;
    owner_->platform_called_ = false;
}
NativeFrameGuard::~NativeFrameGuard()
{
    if (std::uncaught_exceptions() > exceptions_ && owner_->phase_ != OriginalFrames::Phase::Idle)
        owner_->Recover();
    else if (owner_->phase_ == OriginalFrames::Phase::Submitted)
        owner_->phase_ = OriginalFrames::Phase::Idle;
    owner_->busy_ = false;
}
}
