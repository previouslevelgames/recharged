#include "platform/hardware_owner.h"
#include "platform/interrupts.h"
#include "platform/stm_device.h"

#include <aurora/hardware.h>
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
struct Owner {
    std::mutex latch;
    std::thread::id thread;
    SDL_WindowID window{};
    mscharged::platform::StmInput stm{};
    std::uint64_t pending_power{};
    bool ready{}, active{}, overflow{};
};
Owner& State() { static Owner owner; return owner; }

bool SDLCALL Watch(void*, SDL_Event* event) {
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (!state.ready) return true;
    if (event->type != SDL_EVENT_QUIT &&
            !(event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == state.window))
        return true;
    // SDL workers only latch hardware intent. Neither this watcher nor the
    // native owner decides how the original ResetTask responds to that input.
    if (state.pending_power == std::numeric_limits<std::uint64_t>::max()) state.overflow = true;
    else ++state.pending_power;
    return true;
}

void Service() {
    auto& state = State();
    {
        std::lock_guard lock(state.latch);
        // SDK clocks are also called by workers and source critical sections.
        // Such calls must keep ordinary clock behavior and retain pending IRQs.
        if (!state.ready || state.thread != std::this_thread::get_id() || state.active ||
                !mscharged::platform::NativeInterruptsEnabled()) return;
        state.active = true;
    }
    struct Finish { Owner& state; ~Finish() { std::lock_guard lock(state.latch); state.active = false; } } finish{state};
    mscharged::platform::NativeInterruptRead exclusion;
    if (!exclusion) return;
    SDL_PumpEvents();
    mscharged::platform::ServiceWpadSDL();
    {
        std::lock_guard lock(state.latch);
        if (state.overflow) throw std::overflow_error("Native hardware power input capacity exhausted");
        // An occupied STM hardware queue or original one-shot registration
        // retains this intent. Original OSStateTM controls re-registration.
        if (state.pending_power && mscharged::platform::SubmitNativeSTMPower(state.stm)) --state.pending_power;
    }
    mscharged::platform::ServiceNativeSTMDevice();
}
}

namespace mscharged::platform {
void InitializeNativeHardwareOwner(SDL_Window* window, WpadSDLSettings settings) {
    if (!window || SDL_GetWindowID(window) == 0)
        throw std::invalid_argument("Native hardware requires the actual host window");
    auto& state = State();
    {
        std::lock_guard lock(state.latch);
        if (state.ready) throw std::logic_error("Native hardware owner already initialized");
    }
    ConfigureWpadSDL(settings);
    InitializeNativeSTMDevice();
    {
        std::lock_guard lock(state.latch);
        state.thread = std::this_thread::get_id();
        state.window = SDL_GetWindowID(window);
        state.stm = GetNativeSTMInput();
        state.pending_power = 0;
        state.active = state.overflow = false;
        state.ready = true;
    }
    if (!SDL_AddEventWatch(Watch, nullptr)) {
        { std::lock_guard lock(state.latch); state.ready = false; }
        ShutdownNativeSTMDevice();
        throw std::runtime_error(SDL_GetError());
    }
    if (!aurora_register_hardware_service(Service)) {
        { std::lock_guard lock(state.latch); state.ready = false; }
        SDL_RemoveEventWatch(Watch, nullptr);
        ShutdownNativeSTMDevice();
        throw std::logic_error("SDK hardware service already has an owner");
    }
}

void ShutdownNativeHardwareOwner() {
    auto& state = State();
    NativeInterruptGuard exclusion;
    {
        std::lock_guard lock(state.latch);
        if (!state.ready) return;
        if (state.thread != std::this_thread::get_id())
            throw std::logic_error("Native hardware retirement requires its owner");
        if (state.active) throw std::logic_error("Cannot retire an active native hardware callback");
        if (!aurora_unregister_hardware_service(Service))
            throw std::logic_error("Native hardware SDK registration identity changed");
        state.ready = false;
    }
    // Remove watchers before releasing the device state they may have borrowed.
    // A previously loaded SDK function pointer sees ready=false and returns.
    SDL_RemoveEventWatch(Watch, nullptr);
    WPADShutdown();
    ShutdownNativeSTMDevice();
    {
        std::lock_guard lock(state.latch);
        state.pending_power = 0;
        state.window = 0;
        state.stm = {};
        state.thread = {};
        state.overflow = false;
    }
}
}
