#include "credits_movie_hardware.h"
#include "platform/ai.h"
#include "platform/interrupts.h"

#include <aurora/hardware.h>
#include <dolphin/ai.h>

#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
struct Owner {
    std::mutex mutex;
    std::thread::id thread;
    bool registered{}, ready{}, active{};
    void (*service_input)(){};
};
Owner& State() { static Owner owner; return owner; }
}

namespace mscharged::diagnostic {
void ServiceCreditsMovieHardware() {
    auto& owner = State();
    {
        std::lock_guard lock(owner.mutex);
        // Real SDK time/VI services are also called by workers, interrupt
        // callbacks and source critical sections. Retain their latched AI
        // causes; only the original initialization/game thread may deliver.
        if (!owner.ready || owner.thread != std::this_thread::get_id() ||
                owner.active || !platform::NativeInterruptsEnabled()) return;
        owner.active = true;
    }
    struct Finish {
        Owner& owner;
        ~Finish() { std::lock_guard lock(owner.mutex); owner.active = false; }
    } finish{owner};
    platform::ServiceNativeAI();
    // One exact owner, never a replacement SDK registration. VI remains
    // serviced by the SDK after this endpoint returns.
    if (owner.service_input) owner.service_input();
}

void InitializeCreditsMovieHardware() { InitializeCreditsMovieHardware(nullptr); }

void InitializeCreditsMovieHardware(void (*service_input)()) {
    auto& owner = State();
    {
        std::lock_guard lock(owner.mutex);
        if (owner.registered || owner.ready)
            throw std::logic_error("Credits movie hardware already has an owner");
        owner.thread = std::this_thread::get_id();
        owner.service_input = service_input;
        // Never replace a native input/power owner or create a second SDK
        // hardware endpoint. This selected diagnostic has one AI-only owner.
        if (!aurora_register_hardware_service(ServiceCreditsMovieHardware)) {
            owner.thread = {};
            owner.service_input = nullptr;
            throw std::logic_error("The SDK already has a native hardware owner");
        }
        owner.registered = true;
    }
    try {
        if (AICheckInit())
            throw std::logic_error("Credits movie hardware requires an unclaimed host AI device");
        AIInit(nullptr); // Real SDL device; no THPSimple/AX callback is installed.
    } catch (...) {
        std::lock_guard lock(owner.mutex);
        aurora_unregister_hardware_service(ServiceCreditsMovieHardware);
        owner.registered = false;
        owner.thread = {};
        owner.service_input = nullptr;
        throw;
    }
    std::lock_guard lock(owner.mutex);
    owner.ready = true;
}

void ShutdownCreditsMovieHardware() {
    auto& owner = State();
    {
        std::lock_guard lock(owner.mutex);
        if (!owner.registered) return;
        if (owner.thread != std::this_thread::get_id())
            throw std::logic_error("Credits movie hardware must retire on its owner thread");
        if (owner.active)
            throw std::logic_error("Cannot retire an active source audio callback");
        if (!aurora_unregister_hardware_service(ServiceCreditsMovieHardware))
            throw std::logic_error("Credits movie hardware endpoint identity changed");
        owner.ready = false;
        owner.registered = false;
        owner.thread = {};
        owner.service_input = nullptr;
    }
    // Caller has run original MovieStop/Quit while the source module is still
    // live. Stop and drain the true SDL source callbacks before SDK teardown or
    // releasing the memory containing the original static SoundBuffer.
    platform::ShutdownNativeAI();
}
} // namespace mscharged::diagnostic
