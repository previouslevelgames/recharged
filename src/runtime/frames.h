#pragma once
#include "runtime/lighting.h"
#include "runtime/gpu_readback.h"
#include <cstdint>
#include <optional>
#include <thread>

namespace mscharged
{
// A host frame encloses the original game's begin/end/send sequence. Backends
// must drain all references to game memory before its frame allocator advances.
class FrameBackend
{
public:
    virtual ~FrameBackend() = default;
    virtual bool Acquire() = 0;
    virtual void Render() = 0;
    virtual void Finish(bool present) = 0;
    virtual void Drain() = 0;
    virtual void WaitIdle() = 0;
    virtual void Cancel() noexcept = 0;
};

enum class FrameCall { Begin, End, Send, Discard, Finish };
class NativeFrameGuard;
class OriginalFrames
{
    friend class NativeFrameGuard;
    enum class Phase { Idle, Acquired, Collecting, Ended, Submitted };
    FrameBackend& backend_;
    std::thread::id thread_;
    Phase phase_ = Phase::Idle;
    bool live_ = false, busy_ = false;
    bool platform_called_ = false;
    FrameCall call_ = FrameCall::Begin;
    std::uint64_t generation_ = 0;
    void CheckThread() const;
    void CheckPlatformCall(FrameCall call);
    void Recover() noexcept;
public:
    explicit OriginalFrames(FrameBackend& backend);
    ~OriginalFrames();
    OriginalFrames(const OriginalFrames&) = delete;
    OriginalFrames& operator=(const OriginalFrames&) = delete;
    bool Acquire(); // False means no host frame: do not run game tasks.
    void Cancel();  // Abandon an acquired or partially built frame; no presentation.
    void Release();
    void Begin();
    void End();
    void Submit(bool present);
    void Drain();
};

// Production GX backend. The inputs are explicit until game camera/lighting
// managers are initialized. Optional readback is a diagnostic, not a game API.
class AuroraFrames final : public FrameBackend
{
    bool open_ = false;
    void Close(bool present) noexcept;
public:
    float time = 0;
    GameLighting lighting;
    std::optional<nlVector3> camera_position;
    bool read_colours = false;
    ColourSamples colours{};
    bool Acquire() override;
    void Render() override;
    void Finish(bool present) override;
    void Drain() override;
    void WaitIdle() override;
    void Cancel() noexcept override;
};

// Prepared original entry points use this guard for ordering and unwind cleanup.
class NativeFrameGuard
{
    OriginalFrames* owner_;
    int exceptions_;
public:
    explicit NativeFrameGuard(FrameCall call);
    ~NativeFrameGuard();
    NativeFrameGuard(const NativeFrameGuard&) = delete;
    NativeFrameGuard& operator=(const NativeFrameGuard&) = delete;
};
void ResetOriginalFrameState();
void CancelOriginalFrameState();
}
