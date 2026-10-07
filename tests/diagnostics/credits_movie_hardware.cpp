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
    void (*service_device)(void*){};
    void* device_context{};
};
Owner& State() { static Owner owner; return owner; }
}

namespace mscharged::diagnostic {
void ServiceCreditsMovieHardware() {
    auto& owner = State();
    void (*service_device)(void*){};
    void* device_context{};
    void (*service_input)(){};
    {
        std::lock_guard lock(owner.mutex);
        // Real SDK time/VI services are also called by workers, interrupt
        // callbacks and source critical sections. Retain their latched AI
        // causes; only the original initialization/game thread may deliver.
        if (!owner.ready || owner.thread != std::this_thread::get_id() ||
                owner.active || !platform::NativeInterruptsEnabled()) return;
        owner.active = true;
        service_device = owner.service_device;
        device_context = owner.device_context;
        service_input = owner.service_input;
    }
    struct Finish {
        Owner& owner;
        ~Finish() { std::lock_guard lock(owner.mutex); owner.active = false; }
    } finish{owner};
    // Progress acknowledged DSP work before AI and the real requests issued
    // by its source callback afterwards. This delivers actual pending causes;
    // it never calls an AX/THP source callback or modifies source flags.
    if (service_device) service_device(device_context);
    platform::ServiceNativeAI();
    if (service_device) service_device(device_context);
    // One exact owner, never a replacement SDK registration. VI remains
    // serviced by the SDK after this endpoint returns.
    if (service_input) service_input();
}

namespace {
void InitializeOwner(void (*service_input)(), bool initialize_ai) {
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
        if (initialize_ai)
            AIInit(nullptr); // Credits only; original Backend owns AIInit in the audio-init mode.
    } catch (...) {
        std::lock_guard lock(owner.mutex);
        aurora_unregister_hardware_service(ServiceCreditsMovieHardware);
        owner.registered = false;
        owner.thread = {};
        owner.service_input = nullptr;
        throw;
    }
    std::lock_guard lock(owner.mutex);
    owner.ready = true; // Native SDK endpoint registration, not source/AI readiness.
}
} // namespace

void InitializeCreditsMovieHardware() { InitializeCreditsMovieHardware(nullptr); }
void InitializeCreditsMovieHardware(void (*service_input)()) { InitializeOwner(service_input, true); }
void InitializeOriginalGameAudioHardware(void (*service_input)()) { InitializeOwner(service_input, false); }

void BindCreditsMovieDeviceService(void (*service_device)(void*), void* context) {
    auto& owner = State();
    std::lock_guard lock(owner.mutex);
    if (!owner.registered || !owner.ready || owner.thread != std::this_thread::get_id() ||
            owner.active || owner.service_device || !service_device || !context)
        throw std::logic_error("Native audio device needs an idle unique hardware owner binding");
    owner.service_device = service_device;
    owner.device_context = context;
}

void UnbindCreditsMovieDeviceService(void* context) {
    auto& owner = State();
    std::lock_guard lock(owner.mutex);
    if (!owner.registered || !owner.ready || owner.thread != std::this_thread::get_id() ||
            owner.active || !owner.service_device || owner.device_context != context)
        throw std::logic_error("Native audio device must unbind its actual idle borrowed owner");
    owner.service_device = nullptr;
    owner.device_context = nullptr;
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
        if (owner.service_device)
            throw std::logic_error("Borrowed native audio device must unbind before hardware retirement");
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
