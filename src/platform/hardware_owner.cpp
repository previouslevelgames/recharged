#include "platform/hardware_owner.h"
#if defined(MSCHARGED_HOST_SCREENSHOTS)
#include "platform/screenshot_hotkey.h"
#endif
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
#include <vector>

namespace {
struct Owner {
    std::mutex latch;
    std::thread::id thread;
    SDL_WindowID window{};
    mscharged::platform::StmInput stm{};
    std::uint64_t pending_power{};
    bool ready{}, active{}, overflow{}, registered{};
    bool desktop_cadence{};
    bool watched{}, close_retained{}, close_armed{}, close_requested{}, close_submitted{};
    SDL_Window* close_window{};
    std::uint32_t close_type{};
    std::uint64_t close_timestamp{};
    std::chrono::steady_clock::time_point next_input{};
};
Owner& State() { static Owner owner; return owner; }

void ClearInputState(Owner& state) {
    std::lock_guard lock(state.latch);
    state.pending_power = 0;
    state.window = 0;
    state.stm = {};
    state.thread = {};
    state.overflow = false;
    state.watched = state.close_retained = state.close_armed = false;
    state.close_requested = state.close_submitted = false;
    state.close_window = nullptr;
    state.close_type = 0;
    state.close_timestamp = 0;
}

bool SDLCALL Watch(void*, SDL_Event* event) {
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (!state.ready && !state.close_retained) return true;
    if (event->type != SDL_EVENT_QUIT &&
            !(event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == state.window))
        return true;
    if (state.close_retained) {
        // One host close intent, even when both this watch and the initial
        // non-consuming queue peek observe the same event. Later Aurora EXIT
        // is ordinary queue consumption, not a second power request.
        if (!state.close_requested) {
            state.close_requested = true;
            state.close_type = event->type;
            state.close_timestamp = event->common.timestamp;
        }
        return true;
    }
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
        if (state.close_retained && state.close_armed && state.close_requested && !state.close_submitted) {
            if (SDL_GetWindowFromID(state.window) != state.close_window)
                throw std::logic_error("Retained close lost its exact live host window");
            if (!mscharged::platform::IsNativeSTMPowerRemovalConfigured(state.stm))
                throw std::logic_error("Retained close lost its exact configured STM owner");
            if (mscharged::platform::SubmitNativeSTMPower(state.stm)) state.close_submitted = true;
        }
    }
    mscharged::platform::ServiceNativeSTMDevice();
    mscharged::platform::ServiceNativeIOSRequests();
}
}

namespace mscharged::platform {
void RetainNativeHardwareWindowClose(SDL_Window* window) {
    const auto id = window ? SDL_GetWindowID(window) : 0;
    if (!id || SDL_GetWindowFromID(id) != window)
        throw std::invalid_argument("Close retention requires its actual live host window");
    if (!NativeInterruptWaitAllowed() || !NativeInterruptsEnabled())
        throw std::logic_error("Close retention requires an ordinary host owner");
    auto& state = State();
    {
        std::lock_guard lock(state.latch);
        if (state.ready || state.watched || state.close_retained)
            throw std::logic_error("Close retention must precede native input initialization");
        state.thread = std::this_thread::get_id();
        state.window = id;
        state.close_window = window;
        state.close_retained = true;
    }
    if (!SDL_AddEventWatch(Watch, nullptr)) {
        std::lock_guard lock(state.latch);
        state.close_retained = false;
        state.close_window = nullptr;
        state.window = 0;
        state.thread = {};
        throw std::runtime_error(SDL_GetError());
    }
    { std::lock_guard lock(state.latch); state.watched = true; }
    // AddEventWatch does not replay earlier queue entries. Peek only these
    // genuine event kinds after installing the watch; overlaps coalesce.
    // Nothing is removed, filtered, rewritten or re-posted.
    auto peek = [&](std::uint32_t type) {
        const int count = SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, type, type);
        if (count < 0) throw std::runtime_error(SDL_GetError());
        if (!count) return;
        std::vector<SDL_Event> events(static_cast<std::size_t>(count));
        const int found = SDL_PeepEvents(events.data(), count, SDL_PEEKEVENT, type, type);
        if (found < 0) throw std::runtime_error(SDL_GetError());
        for (int i = 0; i != found; ++i) Watch(nullptr, &events[static_cast<std::size_t>(i)]);
    };
    // On failure the actual installed watcher/intent stays owned, rather than
    // silently dropping a close; explicit native retirement can remove it.
    peek(SDL_EVENT_QUIT);
    peek(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
}

void ArmNativeHardwareWindowClose() {
    if (!NativeInterruptWaitAllowed() || !NativeInterruptsEnabled())
        throw std::logic_error("Close arm requires an ordinary owning-thread boundary");
    auto& state = State();
    std::lock_guard lock(state.latch);
    if (!state.ready || !state.close_retained || state.active ||
            state.thread != std::this_thread::get_id() ||
            SDL_GetWindowFromID(state.window) != state.close_window)
        throw std::logic_error("Close arm requires its inactive live native window/input owner");
    if (!IsNativeSTMPowerRemovalConfigured(state.stm))
        throw std::logic_error("Close arm requires the matching installed STM power-removal policy");
    state.close_armed = true;
}

NativeHardwareWindowCloseStatus GetNativeHardwareWindowCloseStatus() {
    auto& state = State();
    std::lock_guard lock(state.latch);
    return {state.close_retained,state.close_armed,state.close_requested,state.close_submitted,
        state.close_retained ? state.window : 0,state.close_type,state.close_timestamp,
        state.close_retained ? state.stm.generation : 0};
}

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
        if (state.close_retained && (state.thread != std::this_thread::get_id() ||
                state.close_window != window || SDL_GetWindowFromID(state.window) != window))
            throw std::logic_error("Native input cannot replace its retained close window lifetime");
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
    bool watched;
    { std::lock_guard lock(state.latch); watched = state.watched; }
    if (!watched && !SDL_AddEventWatch(Watch, nullptr)) {
        { std::lock_guard lock(state.latch); state.ready = false; }
        SetNativeThreadWaitService(nullptr);
        ShutdownNativeAlarms();
        ShutdownDesktopWpad();
        throw std::runtime_error(SDL_GetError());
    }
    { std::lock_guard lock(state.latch); state.watched = true; }
#if defined(MSCHARGED_HOST_SCREENSHOTS)
    InitializeScreenshotHotkey(window);
#endif
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
    bool early_only = false;
    {
        std::lock_guard lock(state.latch);
        if (!state.ready && !state.close_retained) return;
        if (state.thread != std::this_thread::get_id() || state.active || state.registered)
            throw std::logic_error("Native input retirement requires its inactive, unregistered owner");
        if (!state.ready) {
            // Explicit retirement of the optional pre-input host stage. No
            // source module/device ownership is initialized by retention.
            state.close_retained = false;
            early_only = true;
        }
    }
    if (early_only) {
        SDL_RemoveEventWatch(Watch, nullptr);
        ClearInputState(state);
        return;
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
        state.close_retained = false;
    }
    // Remove watchers before releasing the device state they may have borrowed.
    // A previously loaded SDK function pointer sees ready=false and returns.
    SDL_RemoveEventWatch(Watch, nullptr);
#if defined(MSCHARGED_HOST_SCREENSHOTS)
    ShutdownScreenshotHotkey();
#endif
    ShutdownNativeAlarms();
    ShutdownDesktopWpad();
    WPADShutdown();
    ClearInputState(state);
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
