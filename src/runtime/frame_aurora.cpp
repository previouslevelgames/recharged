#include "runtime/frames.h"
#include "runtime/views.h"
#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace mscharged
{
bool AuroraFrames::Acquire()
{
    if (open_) throw std::logic_error("Aurora frame is already acquired");
    return open_ = aurora_begin_frame();
}
void AuroraFrames::Render()
{
    if (!open_) throw std::logic_error("Rendering requires an acquired Aurora frame");
    DispatchOriginalViews(time, lighting, camera_position ? &*camera_position : nullptr);
    GXDrawDone();
}
void AuroraFrames::Close(bool present) noexcept
{
    open_ = false;
    if (present) aurora_end_frame();
    else aurora_end_frame_no_present();
}
void AuroraFrames::Finish(bool present)
{
    if (!open_) throw std::logic_error("Aurora frame is not acquired");
    if (read_colours) colours = EndFrameAndReadColours([&] { Close(present); });
    else Close(present);
}
void AuroraFrames::Drain() { AuroraGXSync(); aurora::gfx::synchronize(); }
void AuroraFrames::WaitIdle()
{
    if (open_) throw std::logic_error("GPU completion requires closing the current host frame first");
    Drain();
    auto done = std::make_shared<std::atomic_int>(0);
    aurora::gfx::queue().OnSubmittedWorkDone(wgpu::CallbackMode::AllowSpontaneous,
        [done](wgpu::QueueWorkDoneStatus status, wgpu::StringView) {
            *done = status == wgpu::QueueWorkDoneStatus::Success ? 1 : -1;
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!*done && std::chrono::steady_clock::now() < deadline)
    {
        aurora::gfx::device().Tick();
        SDL_Delay(1);
    }
    if (*done != 1) throw std::runtime_error("Graphics queue completion failed or timed out");
}
void AuroraFrames::Cancel() noexcept
{
    if (open_) Close(false);
    AuroraGXSync();
    aurora::gfx::synchronize();
}
}
