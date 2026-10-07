#include "platform/hardware_owner.h"
#include "platform/alarms.h"
#include "platform/interrupts.h"
#include "platform/stm_device.h"
#include "platform/ios_device.h"
#include "platform/thread_queues.h"

#include <aurora/hardware.h>
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <cstdint>
#include <chrono>
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
    bool ready{}, active{}, overflow{}, registered{};
    bool desktop_cadence{};
    std::chrono::steady_clock::time_point next_input{};
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
    mscharged::platform::ServiceNativeAlarms();
    const auto now = std::chrono::steady_clock::now();
    if (!state.desktop_cadence || now >= state.next_input) {
        state.next_input = now + std::chrono::milliseconds(10);
        SDL_PumpEvents();
        mscharged::platform::ServiceDesktopWpad();
        mscharged::platform::ServiceWpadSDL();
    }
    {
        std::lock_guard lock(state.latch);
        if (state.overflow) throw std::overflow_error("Native hardware power input capacity exhausted");
        // An occupied STM hardware queue or original one-shot registration
        // retains this intent. Original OSStateTM controls re-registration.
        if (state.pending_power && mscharged::platform::SubmitNativeSTMPower(state.stm)) --state.pending_power;
    }
    mscharged::platform::ServiceNativeSTMDevice();
    mscharged::platform::ServiceNativeIOSRequests();
}
}

namespace mscharged::platform {
void InitializeNativeHardwareInput(SDL_Window* window, WpadSDLSettings settings,
    StmInput borrowed_stm, DesktopWpadSettings desktop) {
    if (!window || SDL_GetWindowID(window) == 0)
        throw std::invalid_argument("Native hardware requires the actual host window");
    if (!borrowed_stm.generation || GetNativeSTMInput().generation != borrowed_stm.generation)
        throw std::logic_error("Native input requires the exact initialized STM device incarnation");
    auto& state = State();
    {
        std::lock_guard lock(state.latch);
        if (state.ready) throw std::logic_error("Native hardware input already initialized");
    }
    ConfigureWpadSDL(settings);
    InitializeDesktopWpad(window, desktop);
    try { InitializeNativeAlarms(); }
    catch (...) { ShutdownDesktopWpad(); throw; }
    try {
        // Original waits may deschedule this owner. Continue only real native
        // interrupt delivery there, without desktop scanout or game updates.
        const auto prior = SetNativeThreadWaitService(aurora_service_hardware_interrupts);
        if (prior) {
            SetNativeThreadWaitService(prior);
            throw std::logic_error("Native owner wait service already has a borrower");
        }
    } catch (...) {
        ShutdownNativeAlarms();
        ShutdownDesktopWpad();
        throw;
    }
    {
        std::lock_guard lock(state.latch);
        state.thread = std::this_thread::get_id();
        state.window = SDL_GetWindowID(window);
        state.stm = borrowed_stm;
        state.pending_power = 0;
        state.active = state.overflow = state.registered = false;
        state.desktop_cadence = desktop.keyboard || desktop.gamepads || desktop.mouse;
        state.next_input = {};
        state.ready = true;
    }
    if (!SDL_AddEventWatch(Watch, nullptr)) {
        { std::lock_guard lock(state.latch); state.ready = false; }
        SetNativeThreadWaitService(nullptr);
        ShutdownNativeAlarms();
        ShutdownDesktopWpad();
        throw std::runtime_error(SDL_GetError());
    }
}

void ServiceNativeHardwareInput() { Service(); }

void InitializeNativeHardwareOwner(SDL_Window* window, WpadSDLSettings settings) {
    { std::lock_guard lock(State().latch);
      if (State().ready) throw std::logic_error("Native hardware owner already initialized"); }
    InitializeNativeSTMDevice();
    try { InitializeNativeHardwareInput(window, settings, GetNativeSTMInput()); }
    catch (...) { ShutdownNativeSTMDevice(); throw; }
    if (!aurora_register_hardware_service(Service)) {
        ShutdownNativeHardwareInput();
        ShutdownNativeSTMDevice();
        throw std::logic_error("SDK hardware service already has an owner");
    }
    std::lock_guard lock(State().latch);
    State().registered = true;
}

void ShutdownNativeHardwareInput() {
    auto& state = State();
    {
        std::lock_guard lock(state.latch);
        if (!state.ready) return;
        if (state.thread != std::this_thread::get_id() || state.active || state.registered)
            throw std::logic_error("Native input retirement requires its inactive, unregistered owner");
    }
    // Source joins/detaches must have ended before device, descriptor, callback
    // image or arena backing retires. Physical joins cannot hold IRQ exclusion.
    DrainNativeThreadLifetimes();
    NativeInterruptGuard exclusion;
    {
        std::lock_guard lock(state.latch);
        if (!state.ready) return;
        if (state.thread != std::this_thread::get_id())
            throw std::logic_error("Native hardware retirement requires its owner");
        if (state.active) throw std::logic_error("Cannot retire an active native hardware callback");
        if (state.registered)
            throw std::logic_error("Retire the exact native SDK owner before borrowed input");
        const auto prior = SetNativeThreadWaitService(nullptr);
        if (prior != aurora_service_hardware_interrupts) {
            SetNativeThreadWaitService(prior);
            throw std::logic_error("Native owner wait service identity changed");
        }
        state.ready = false;
    }
    // Remove watchers before releasing the device state they may have borrowed.
    // A previously loaded SDK function pointer sees ready=false and returns.
    SDL_RemoveEventWatch(Watch, nullptr);
    ShutdownNativeAlarms();
    ShutdownDesktopWpad();
    WPADShutdown();
    {
        std::lock_guard lock(state.latch);
        state.pending_power = 0;
        state.window = 0;
        state.stm = {};
        state.thread = {};
        state.overflow = false;
    }
}

void ShutdownNativeHardwareOwner() {
    auto& state = State();
    {
        std::lock_guard lock(state.latch);
        if (!state.ready) return;
        if (state.thread != std::this_thread::get_id() || state.active || !state.registered)
            throw std::logic_error("Native hardware retirement requires its inactive SDK owner");
    }
    // Reject unfinished source lifetimes before changing hardware registration.
    DrainNativeThreadLifetimes();
    {
        NativeInterruptGuard exclusion;
        std::lock_guard lock(state.latch);
        if (!state.ready) return;
        if (state.thread != std::this_thread::get_id() || state.active || !state.registered)
            throw std::logic_error("Native hardware retirement requires its inactive SDK owner");
        if (!aurora_unregister_hardware_service(Service))
            throw std::logic_error("Native hardware SDK registration identity changed");
        state.registered = false;
    }
    ShutdownNativeHardwareInput();
    ShutdownNativeSTMDevice();
}

}
