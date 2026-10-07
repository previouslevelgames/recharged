#include "platform/desktop_wpad.h"
#include "platform/alarms.h"
#include "platform/hardware_owner.h"
#include "platform/ai.h"
#include "platform/interrupts.h"
#include "platform/stm_device.h"
#include "credits_movie_hardware.h"
#include <aurora/hardware.h>
#include <dolphin/ai.h>
#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>
#include <aurora/aurora.h>
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();

namespace {
using namespace mscharged;
unsigned checks{}, audio_callbacks{}, alarm_callbacks{}, connected{}, samples{};
std::thread::id owner;
SDL_Window* window{};
OSContext* interrupted{};
OSAlarm alarm{};
alignas(32) std::array<s16, 192> pcm{};
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F fn, const char* message) {
    bool rejected=false; try { fn(); } catch (const std::logic_error&) { rejected=true; }
    Check(rejected,message);
}
void Occupied() {}
void Audio() {
    Check(owner == std::this_thread::get_id() && !platform::NativeInterruptsEnabled(),
        "Audio callback crossed native owner/exclusion");
    ++audio_callbacks;
    const auto before=audio_callbacks;
    const auto old=OSEnableInterrupts();
    (void)OSGetTime();
    diagnostic::ServiceCreditsMovieHardware();
    Check(audio_callbacks==before,"Input composition made audio recursive");
    OSRestoreInterrupts(old);
    AIInitDMA(reinterpret_cast<std::uintptr_t>(pcm.data()),sizeof(pcm));
}
void Connect(s32, WPADResult result) {
    Check(owner==std::this_thread::get_id()&&!platform::NativeInterruptsEnabled(),
        "WPAD connection callback crossed native owner/exclusion");
    connected += result==WPAD_ERR_OK;
}
void Sample(s32) {
    Check(owner==std::this_thread::get_id()&&!platform::NativeInterruptsEnabled(),
        "WPAD sampling callback crossed native owner/exclusion");
    ++samples;
}
void Alarm(OSAlarm* request, OSContext* context) {
    Check(request == &alarm && context == interrupted,
        "Shared owner lost the actual source timer or interrupted SDK context");
    Check(owner == std::this_thread::get_id() && !platform::NativeInterruptsEnabled(),
        "Timer callback crossed native owner/exclusion");
    ++alarm_callbacks;
    Check(platform::ServiceNativeAlarms() == 0,
        "Shared hardware owner recursively delivered a timer callback");
}
void WindowEvent(Uint32 type) {
    SDL_Event event{};event.type=type;event.window.windowID=SDL_GetWindowID(window);
    Check(SDL_PushEvent(&event),"Cannot inject actual SDL focus event");
}
void Key(SDL_Scancode code,bool down) {
    SDL_Event event{};event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
    event.key.windowID=SDL_GetWindowID(window);event.key.scancode=code;event.key.down=down;
    Check(SDL_PushEvent(&event),"Cannot inject actual SDL keyboard event");
}
WPADStatus Report(int channel) { WPADStatus report{};WPADRead(channel,&report);return report; }
template<class F> void Until(F fn,const char* message) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!fn()) {
        diagnostic::ServiceCreditsMovieHardware();
        if(std::chrono::steady_clock::now()>=end) {
            std::fprintf(stderr,"Timeout: connects=%u samples=%u channels=%zu, reports err %d/%d buttons %u/%u\n",
                connected,samples,platform::WpadSDLConnectedChannels(),int(Report(0).err),int(Report(1).err),
                unsigned(Report(0).button),unsigned(Report(1).button));
            int count{};auto* ids=SDL_GetGamepads(&count);
            for(int n=0;n<count;++n)std::fprintf(stderr,"SDL device %u %04x:%04x %s\n",ids[n],
                SDL_GetGamepadVendorForID(ids[n]),SDL_GetGamepadProductForID(ids[n]),SDL_GetGamepadNameForID(ids[n]));
            SDL_free(ids);
            throw std::runtime_error(message);
        }
        SDL_Delay(2);
    }
    ++checks;
}
struct GenericPad {
    SDL_JoystickID id{};SDL_Joystick* joystick{};
    GenericPad() {
        SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
        desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.vendor_id=0x1234;desc.product_id=0x4321;
        desc.name="Generated desktop input fixture";desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
        desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
        id=SDL_AttachVirtualJoystick(&desc);Check(id!=0,"Cannot attach actual generic SDL pad");
        joystick=SDL_OpenJoystick(id);Check(joystick,"Cannot open generic SDL fixture handle");
    }
    void A(bool value) { Check(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,value),"Cannot submit actual generic pad A");SDL_UpdateJoysticks(); }
    void Detach() {
        SDL_CloseJoystick(joystick);joystick=nullptr;
        Check(SDL_DetachVirtualJoystick(id),"Cannot detach actual generic SDL pad");id=0;
    }
};
}
int main() {
    using namespace mscharged;
    try {
        owner=std::this_thread::get_id();
        // Preserve the pre-existing host preference. The explicit desktop
        // profile owns raw delivery while source focus decisions stay intact.
        Check(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"0"),"Cannot configure initial raw-input policy");
        Check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD),"Cannot initialize actual SDL dummy devices");
        window=SDL_CreateWindow("Desktop WPAD ownership gate",640,448,SDL_WINDOW_HIDDEN);
        Check(window,"Cannot create actual SDL fixture window");
        aurora::g_config.mem1Size=MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size=64u*1024u*1024u;
        OSInit();
        interrupted=OSGetCurrentContext();
        Check(OSGetArenaLo() && interrupted,"Actual SDK memory/context unavailable");
        platform::InitializeNativeSTMDevice();const auto stm=platform::GetNativeSTMInput();
        Reject([&]{platform::InitializeNativeHardwareInput(window,{0,3},{0},{true,true});},
            "Borrowed input accepted unknown STM incarnation");
        GenericPad pad;
        platform::InitializeNativeHardwareInput(window,{0,3},stm,{true,true});
        Check(SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,false),
            "Desktop profile cannot publish raw release reports after losing focus");
        Reject([&]{platform::InitializeNativeHardwareInput(window,{0,3},stm,{true,true});},
            "Double input initialization replaced virtual device ownership");
        Check(aurora_register_hardware_service(Occupied),"Cannot occupy actual SDK hardware hook");
        Reject([&]{diagnostic::InitializeCreditsMovieHardware(platform::ServiceNativeHardwareInput);},
            "AI/input composition silently replaced SDK owner");
        Check(!AICheckInit()&&aurora_unregister_hardware_service(Occupied),"Collision opened AI or changed owner identity");
        diagnostic::InitializeCreditsMovieHardware(platform::ServiceNativeHardwareInput);
        OSCreateAlarm(&alarm);
        const u32 timer_ticks=OSNanosecondsToTicks(6666667);
        Check(timer_ticks==405000,"Original AudioBackend timer request overflowed");
        OSSetPeriodicAlarm(&alarm,timer_ticks,timer_ticks,Alarm);
        Until([&]{return alarm_callbacks>=3;},
            "The actual shared SDK owner did not deliver source timer callbacks");
        OSCancelAlarm(&alarm);
        WPADInit();for(int n=0;n<4;++n){WPADSetConnectCallback(n,Connect);WPADSetSamplingCallback(n,Sample);}
        WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
        Until([&]{return connected==2&&Report(0).err==WPAD_ERR_OK&&Report(1).err==WPAD_ERR_OK;},
            "Desktop virtual devices failed actual WPAD discovery/report path");
        Check(platform::WpadSDLConnectedChannels()==2,"Generic/keyboard virtual identities duplicated discovery");
        Check(Report(0).button==0&&Report(1).button==0,"Host manufactured initial desktop button presses");
        Check(Report(0).accX==0&&Report(0).accY==0&&Report(0).accZ==100,
            "Neutral core-Wii raw gravity transport changed");
        Key(SDL_SCANCODE_RETURN,true);pad.A(true);
        const auto old=OSDisableInterrupts();const auto before_samples=samples;
        SDL_Delay(12);(void)OSGetTime();diagnostic::ServiceCreditsMovieHardware();
        Check(samples==before_samples&&Report(0).button==0,"Masked owner delivered a desktop source report");
        OSRestoreInterrupts(old);
        Until([&]{return (Report(0).button&WPAD_BUTTON_A)&&(Report(1).button&WPAD_BUTTON_A);},
            "Actual keyboard/controller events failed raw Wii A transport");
        Key(SDL_SCANCODE_UP,true);Key(SDL_SCANCODE_DOWN,true);
        Until([&]{return (Report(0).button&WPAD_BUTTON_UP)!=0;},"Keyboard DPad report did not reach WPAD");
        Check(!(Report(0).button&WPAD_BUTTON_DOWN),"Existing WPAD opposite-button source contract changed");
        WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
        Until([&]{return Report(0).button==0&&Report(1).button==0;},"Focus loss retained stale digital input");
        WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
        Key(SDL_SCANCODE_RETURN,true);
        bool foreign_rejected=false;
        std::thread worker([&]{
            (void)OSGetTime();diagnostic::ServiceCreditsMovieHardware();
            try{platform::ShutdownNativeHardwareInput();}catch(const std::logic_error&){foreign_rejected=true;}
        });worker.join();
        Check(foreign_rejected&&Report(0).button==0,"Foreign owner delivered/retired desktop input");
        Until([&]{return Report(0).button&WPAD_BUTTON_A;},"Owner failed to service retained keyboard input");
        AIRegisterDMACallback(Audio);AIInitDMA(reinterpret_cast<std::uintptr_t>(pcm.data()),sizeof(pcm));AIStartDMA();
        Until([&]{(void)OSGetTime();return audio_callbacks>=3;},"One SDK owner did not compose actual AI and input");
        Until([&]{return platform::GetNativeAIStatus().consumed_blocks!=0;},"Actual SDL dummy audio did not consume retained PCM");
        pad.Detach();
        Until([&]{return platform::WpadSDLConnectedChannels()==1;},"Generic hot-unplug retained its virtual raw-device lease");
        AIStopDMA();diagnostic::ShutdownCreditsMovieHardware();
        const auto stopped=platform::GetNativeAIStatus();
        Check(!stopped.initialized&&!stopped.retained_blocks,"Hardware owner retirement retained audio/device callbacks");
        platform::ShutdownNativeHardwareInput();
        Check(!SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,true),
            "Desktop profile did not restore prior raw-input policy");
        Check(WPADGetStatus()==WPAD_LIB_STATUS_0&&platform::GetNativeSTMInput().generation==stm.generation,
            "Borrowed input retirement changed STM owner or retained WPAD callbacks");
        platform::ShutdownNativeSTMDevice();
        Check(aurora_register_hardware_service(Occupied)&&aurora_unregister_hardware_service(Occupied),
            "Input composition left a duplicate SDK owner");
        // Separate legacy stand-alone owner remains supported and owns STM itself.
        platform::InitializeNativeHardwareOwner(window,{0,3});
        WPADInit();platform::ServiceNativeHardwareInput();
        OSCreateAlarm(&alarm);
        OSSetAlarm(&alarm,OSSecondsToTicks(1),Alarm);
        platform::ShutdownNativeHardwareOwner();
        Check(WPADGetStatus()==WPAD_LIB_STATUS_0,"Standalone owner regression retained WPAD");
        Check(!alarm.handler && platform::ServiceNativeAlarms()==0,
            "Shared owner retirement retained a borrowed source timer");
        SDL_DestroyWindow(window);window=nullptr;SDL_Quit();
        AuroraOSShutdown();
        std::printf("Desktop WPAD/one-owner hardware: %u checks, %u source-shaped WPAD samples, %u real AI callbacks, %u real timer callbacks; genuine SDL/one prepared SDK, no original FE/game readiness claim.\n",checks,samples,audio_callbacks,alarm_callbacks);
        return 0;
    }catch(const std::exception& error){
        try{diagnostic::ShutdownCreditsMovieHardware();}catch(...){}
        try{platform::ShutdownNativeHardwareInput();}catch(...){}
        try{platform::ShutdownNativeSTMDevice();}catch(...){}
        if(window)SDL_DestroyWindow(window);SDL_Quit();
        AuroraOSShutdown();
        std::fprintf(stderr,"Desktop WPAD/one-owner hardware: %s\n",error.what());return 1;
    }
}
