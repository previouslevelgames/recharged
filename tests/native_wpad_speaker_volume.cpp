#include "platform/interrupts.h"
#include "platform/wpad_sdl.h"

#include <revolution/os.h>
#include <revolution/wpad/WPAD.h>
#include <SDL3/SDL.h>

#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
unsigned checks = 0;
unsigned completions = 0;
std::thread::id owner;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template<class F> void Reject(F&& function, const char* message) {
    bool rejected = false;
    try { function(); }
    catch (const std::logic_error&) { rejected = true; }
    Check(rejected, message);
}

void SpeakerCompletion(s32 channel, WPADResult result) {
    Check(channel == 0 && static_cast<int>(result) == -1,
        "Absent device speaker request did not retain the original ENODEV result");
    Check(std::this_thread::get_id() == owner,
        "Speaker failure completion left the actual WPAD owner");
    ++completions;
}

void Configure(std::uint8_t volume) {
    mscharged::platform::WpadSDLSettings settings{0, 3, false, false};
    settings.speaker_volume = volume;
    mscharged::platform::ConfigureWpadSDL(settings);
}
}

int main() {
    try {
        owner = std::this_thread::get_id();
        Check(SDL_Init(SDL_INIT_EVENTS), "Cannot initialize actual SDL event owner");
        Check(WPADGetSpeakerVolume() == 0, "Cold WPAD byte did not retain static zero initialization");
        WPADSetSpeakerVolume(255);
        Check(WPADGetSpeakerVolume() == 127, "Cold preference write did not clamp at literal 127");
        mscharged::platform::ConfigureWpadSDL({0, 3, false, false});
        WPADInit();
        Check(WPADGetSpeakerVolume() == 89,
            "Initialization did not use original absent-BT.SPKV default 89");
        for (unsigned value = 0; value != 256; ++value) {
            const auto mask = OSDisableInterrupts();
            WPADSetSpeakerVolume(static_cast<u8>(value));
            Check(WPADGetSpeakerVolume() == (value > 127 ? 127 : value),
                "Native preference differs from the independent literal-byte range oracle");
            Check(!mscharged::platform::NativeInterruptsEnabled(),
                "Preference query/write enabled a retained source mask");
            OSRestoreInterrupts(mask);
        }
        Check(mscharged::platform::NativeInterruptsEnabled(),
            "Preference query/write changed the source caller's restored mask");
        WPADSetSpeakerVolume(54);
        Reject([] { Configure(32); }, "Active WPAD allowed replacement of its staged preferences");
        Check(WPADGetSpeakerVolume() == 54, "Failed active reconfiguration changed the live byte");

        unsigned rejected = 0;
        std::thread foreign([&] {
            try { (void)WPADGetSpeakerVolume(); }
            catch (const std::logic_error&) { ++rejected; }
            try { WPADSetSpeakerVolume(33); }
            catch (const std::logic_error&) { ++rejected; }
        });
        foreign.join();
        Check(rejected == 2 && WPADGetSpeakerVolume() == 54,
            "Foreign thread queried/mutated a live SDL owner preference");

        Check(!WPADIsSpeakerEnabled(0) && !WPADCanSendStreamData(0),
            "Preference updates supplied unsupported speaker readiness");
        Check(static_cast<int>(WPADControlSpeaker(0, WPAD_SPEAKER_ON, SpeakerCompletion)) == -1,
            "Preference supplied a successful speaker operation without an actual device");
        Check(completions == 1 && WPADGetSpeakerVolume() == 54,
            "Actual failed-request completion changed the speaker preference");
        WPADShutdown();
        Check(WPADGetSpeakerVolume() == 54, "Shutdown rewrote the original retained preference byte");

        Configure(0);
        WPADInit();
        Check(WPADGetSpeakerVolume() == 0, "Explicit zero preference was replaced by a default");
        WPADSetSpeakerVolume(127);
        WPADShutdown();
        WPADInit();
        Check(WPADGetSpeakerVolume() == 0,
            "New WPAD incarnation did not restore the staged preference");
        WPADShutdown();
        Configure(255);
        WPADInit();
        Check(WPADGetSpeakerVolume() == 127,
            "Staged SC byte above 127 did not retain the original clamp");
        Check(!WPADIsSpeakerEnabled(0) && !WPADCanSendStreamData(0),
            "Reinitialization supplied unsupported speaker readiness");
        WPADShutdown();
        SDL_Quit();
        std::printf("Native WPAD speaker preference: %u checks; real SDL ownership/range/reinit pass, no physical speaker output.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native WPAD speaker preference: %s\n", error.what());
        try { WPADShutdown(); } catch (...) {}
        SDL_Quit();
        return 1;
    }
}
