#pragma once
#include <functional>
#include <memory>

namespace mscharged
{
class OriginalFrames;
class GraphicsFrameTasks;
// Qualified glStartup stages only. The host session and initialized game arenas
// outlive this owner; asset owners must be released before Release(). Video setup
// is a required real host service, not a replacement for full glplatStartup.
class GraphicsStartup
{
    friend class GraphicsFrameTasks;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void Ready() const;
public:
    GraphicsStartup(unsigned width, unsigned height,
        const std::function<void()>& configure_video, void (*drain)());
    ~GraphicsStartup();
    GraphicsStartup(const GraphicsStartup&) = delete;
    GraphicsStartup& operator=(const GraphicsStartup&) = delete;
    void Release();
};

struct GraphicsFrameCallbacks
{
    // Begin runs after glBeginFrame; end runs between glEndFrame/glSendFrame.
    // Every callback is required. Delta is the original scheduler's clamped,
    // dilated value; deterministic diagnostics may explicitly choose their clock.
    std::function<void(float)> begin, update, render, end;
};
// Owns an exclusive original scheduler in state 1. Four selected callbacks use
// original main's begin/world-update/game-render/end priorities. Full original
// game tasks, transitions, movie playback and other services remain unselected.
// Startup and frames must outlive this owner. Failure requires Release/recreate.
class GraphicsFrameTasks
{
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    GraphicsFrameTasks(GraphicsStartup& startup, OriginalFrames& frames,
        GraphicsFrameCallbacks callbacks);
    ~GraphicsFrameTasks();
    GraphicsFrameTasks(const GraphicsFrameTasks&) = delete;
    GraphicsFrameTasks& operator=(const GraphicsFrameTasks&) = delete;
    void RunAcquired(); // Caller acquires host frame and performs deferred GXInit.
    void Release();
};
}
