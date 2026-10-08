#include "platform/hardware_owner.h"
#include "platform/alarms.h"
#include "platform/interrupts.h"
#include "platform/stm_device.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <revolution/os.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();

namespace {
using namespace mscharged::platform;
unsigned checks{}, powers{}, alarms{};
std::thread::id owner;
void Check(bool value, const char* error) { ++checks; if (!value) throw std::runtime_error(error); }
template<class F> void Reject(F f, const char* error) {
    bool rejected = false;
    try { f(); } catch (const std::logic_error&) { rejected = true; }
    Check(rejected, error);
}
void Power() {
    Check(std::this_thread::get_id() == owner && !NativeInterruptsEnabled(),
        "Real original OSStateTM callback lost its owner/IRQ boundary");
    ++powers;
}
void Alarm(OSAlarm*, OSContext*) { ++alarms; }
void NeverRemove(void*, const NativeSTMPowerRequest&) {
    throw std::logic_error("Window intent leaf must not request terminal removal");
}
void Close(SDL_Window* window) {
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    event.window.windowID = SDL_GetWindowID(window);
    Check(SDL_PushEvent(&event), "Real SDL window-close queue rejected the fixture request");
}
void Quit() {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    Check(SDL_PushEvent(&event), "Real SDL quit queue rejected the fixture request");
}
int Count(std::uint32_t type) {
    const auto count = SDL_PeepEvents(nullptr,0,SDL_PEEKEVENT,type,type);
    Check(count >= 0, "Cannot observe actual SDL queue");
    return count;
}
}

int main() {
    SDL_Window* window{};
    SDL_Window* other{};
    try {
        owner = std::this_thread::get_id();
        Check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD), "SDL dummy foundation unavailable");
        window = SDL_CreateWindow("Owned close retention fixture",640,480,SDL_WINDOW_HIDDEN);
        other = SDL_CreateWindow("Foreign close retention fixture",320,240,SDL_WINDOW_HIDDEN);
        Check(window && other, "Actual SDL windows unavailable");
        aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size = 64u * 1024u * 1024u;
        OSInit();
        InitializeNativeSTMDevice();
        const auto stm = GetNativeSTMInput();
        Check(__OSInitSTM(), "Whole original OSStateTM did not initialize actual STM endpoints");
        Check(!IsNativeSTMPowerRemovalConfigured(stm), "Default installed an invented terminal policy");

        Close(other);
        RetainNativeHardwareWindowClose(window);
        Check(!GetNativeHardwareWindowCloseStatus().requested, "Foreign queued close became owned intent");
        ShutdownNativeHardwareInput();
        Check(!GetNativeHardwareWindowCloseStatus().retaining, "Pre-input watch lifetime was not retired");
        Close(window); // This genuine event is queued before our watch exists.
        const auto queued = Count(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
        RetainNativeHardwareWindowClose(window);
        auto state = GetNativeHardwareWindowCloseStatus();
        Check(state.retaining && state.requested && !state.armed && !state.submitted &&
                state.window_id == SDL_GetWindowID(window) &&
                state.event_type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && state.event_timestamp,
            "Pre-watch queued close was not retained with actual SDL metadata");
        Check(Count(SDL_EVENT_WINDOW_CLOSE_REQUESTED) == queued, "Queue adoption removed or re-posted an event");
        Reject([&] { RetainNativeHardwareWindowClose(window); }, "Duplicate retention replaced its live watch");
        Reject([&] { InitializeNativeHardwareInput(other,{0,3},stm); }, "Foreign input replaced retained window");
        InitializeNativeHardwareInput(window,{0,3},stm);
        for (int i=0;i!=3;++i) ServiceNativeHardwareInput();
        state = GetNativeHardwareWindowCloseStatus();
        Check(state.requested && !state.submitted && state.stm_generation == stm.generation,
            "Pre-arm service consumed early close intent");
        Reject(ArmNativeHardwareWindowClose, "Close arm accepted an absent actual terminal policy");

        // Deferral applies only to SDL close: physical power/reset still go
        // immediately through unchanged whole OSStateTM decisions/callbacks.
        Check(OSSetPowerCallback(Power), "Original default callback predecessor missing");
        Check(SubmitNativeSTMPower(stm), "Physical power request rejected by its actual device");
        ServiceNativeHardwareInput();
        Check(powers == 1 && !GetNativeHardwareWindowCloseStatus().submitted,
            "Optional window lifecycle deferred physical power");
        OSSetPowerCallback(Power);
        Check(SetNativeSTMResetButton(stm,true), "Physical reset level rejected");
        ServiceNativeHardwareInput();
        Check(OSGetResetButtonState() && powers == 1, "Original physical reset behavior changed");
        Check(SetNativeSTMResetButton(stm,false), "Physical reset release rejected");
        OSAlarm alarm{};
        OSCreateAlarm(&alarm);
        OSSetAlarm(&alarm,1,Alarm);
        ServiceNativeHardwareInput();
        Check(alarms == 1 && !GetNativeHardwareWindowCloseStatus().submitted,
            "Disarmed close interrupted actual timer servicing");

        int policy_owner{};
        ConfigureNativeSTMPowerRemoval({&policy_owner,NeverRemove});
        Check(IsNativeSTMPowerRemovalConfigured(stm) &&
                !IsNativeSTMPowerRemovalConfigured({stm.generation+1}),
            "Policy query lost actual STM incarnation");
        std::atomic<bool> rejected{};
        std::thread foreign([&] {
            try { ArmNativeHardwareWindowClose(); } catch (const std::logic_error&) { rejected = true; }
        });
        foreign.join();
        Check(rejected, "Foreign thread armed owner delivery");
        const auto mask = OSDisableInterrupts();
        Reject(ArmNativeHardwareWindowClose, "Masked owner armed SDL conversion");
        Check(!NativeInterruptsEnabled(), "Rejected arm changed the source mask");
        OSRestoreInterrupts(mask);
        ArmNativeHardwareWindowClose();
        Check(GetNativeHardwareWindowCloseStatus().armed && powers == 1,
            "Arm delivered a source callback synchronously");
        Quit(); Close(window); Close(other); // Watch/queued observations coalesce.
        const auto masked = OSDisableInterrupts();
        ServiceNativeHardwareInput();
        Check(powers == 1 && !GetNativeHardwareWindowCloseStatus().submitted,
            "Masked owner consumed armed intent");
        OSRestoreInterrupts(masked);
        ServiceNativeHardwareInput();
        Check(powers == 2 && GetNativeHardwareWindowCloseStatus().submitted,
            "Armed service did not deliver exactly one genuine source notification");
        OSSetPowerCallback(Power); // Re-register through the original source API.
        Quit();
        for (int i=0;i!=3;++i) ServiceNativeHardwareInput();
        Check(powers == 2, "Repeated watch/queue/host-close observations duplicated delivery");
        Check(Count(SDL_EVENT_WINDOW_CLOSE_REQUESTED) == queued+2 && Count(SDL_EVENT_QUIT) == 2,
            "Service changed retained SDL close queue contents");
        // Ordinary host polling consumes the real events; it does not feed a
        // second native power request or mutate their retained metadata.
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {}
        ServiceNativeHardwareInput();
        Check(powers == 2, "Later host queue consumption duplicated native power");
        ShutdownNativeHardwareInput();
        Reject(ArmNativeHardwareWindowClose, "Retired window/input lifetime could arm");

        // Legacy callers retain the original immediate window-power path.
        InitializeNativeHardwareInput(window,{0,3},stm);
        OSSetPowerCallback(Power);
        Close(other); ServiceNativeHardwareInput();
        Check(powers == 2, "Legacy owner accepted a foreign window close");
        Close(window); ServiceNativeHardwareInput();
        Check(powers == 3 && !GetNativeHardwareWindowCloseStatus().retaining,
            "Default caller acquired close deferral or lost immediate delivery");
        ShutdownNativeHardwareInput();

        // Deliberate stale borrowed-device negative, with no source restart or
        // callback acceptance claim for a new STM backend incarnation.
        RetainNativeHardwareWindowClose(window);
        InitializeNativeHardwareInput(window,{0,3},stm);
        Check(__OSUnRegisterStateEvent() == 0, "Source event receiver did not retire before negative");
        ShutdownNativeSTMDevice();
        InitializeNativeSTMDevice();
        const auto next = GetNativeSTMInput();
        ConfigureNativeSTMPowerRemoval({&policy_owner,NeverRemove});
        Check(next.generation != stm.generation, "Retired STM incarnation was reused");
        Reject(ArmNativeHardwareWindowClose, "Stale borrowed STM generation armed delivery");
        ShutdownNativeHardwareInput();
        ShutdownNativeSTMDevice();
        SDL_DestroyWindow(other); other=nullptr;
        SDL_DestroyWindow(window); window=nullptr;
        SDL_Quit(); AuroraOSShutdown();
        std::printf("Native window-close retention PASS: %u checks, actual SDL queue/watch and whole OSStateTM; no partial main or terminal removal acceptance.\n",checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Native window-close retention failed after %u checks: %s\n",checks,error.what());
        return 1;
    }
}
