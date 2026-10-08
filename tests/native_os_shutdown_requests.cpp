#include "platform/os_shutdown_requests.h"
#include "platform/os_shutdown_requests_abi.h"
#include "platform/filesystem_boot.h"
#include "platform/interrupts.h"
#include "platform/ios_device.h"
#include "platform/path.h"

#include <aurora/dvd.h>
#include <aurora/aurora.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

void AuroraOSShutdown();
namespace aurora { extern AuroraConfig g_config; }
namespace {
using namespace mscharged;
using namespace mscharged::platform;
unsigned checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void Reject(F fn, const char* message) {
    bool rejected{};
    try { fn(); } catch (const std::logic_error&) { rejected=true; }
    Check(rejected, message);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Actual filesystem catalog is absent");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Failures(const NativeFilesystemSettings& settings) {
    const auto catalog = Read(settings.root/"metadata.txt");
    const auto disc = *DVDGetCurrentDiskID();
    const auto* context = OSGetCurrentContext();
    const std::array<void(*)(), 7> calls{{
        +[]{__OSReboot(0,0);},
        +[]{__OSReboot(0x12345678,0xfedcba98);},
        __OSLaunchMenu, __OSRelaunchTitle, __VISetRGBModeImm,
        +[]{(void)__PADDisableRecalibration(FALSE);},
        +[]{(void)__PADDisableRecalibration(TRUE);}
    }};
    for (bool masked : {false,true}) {
        const auto prior = masked ? OSDisableInterrupts() : TRUE;
        for (const auto call : calls) {
            Reject(call, "Unsupported platform request returned success");
            Check(NativeInterruptsEnabled() == !masked,
                "Unsupported platform request changed the original mask");
            Check(OSGetCurrentContext() == context,
                "Unsupported platform request changed the original context");
            Check(OSGetAppType() == 0x80,
                "Unsupported platform request mutated source boot metadata");
            Check(std::memcmp(DVDGetCurrentDiskID(), &disc, sizeof(disc)) == 0,
                "Unsupported platform request changed actual disc identity");
            const auto ios = GetNativeIOSStatus();
            Check(!ios.pending && !ios.active,
                "Unsupported platform request created an IOS completion");
        }
        if (masked) OSRestoreInterrupts(prior);
    }
    Check(Read(settings.root/"metadata.txt") == catalog,
        "Unsupported platform request changed persistent storage");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::invalid_argument("Expected real synthetic Wii image and backing directory");
        Reject([]{(void)OSGetAppType();}, "Absent app identity implied disc boot");
        // Configure the same real OS memory banks used by the existing pure
        // native alarm/audio-stop fixtures. No window, GX or source field seed.
        aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size = 64u*1024u*1024u;
        OSInit();
        Check(OSGetArenaLo() && OSGetArenaHi(), "Actual native OS arenas are absent");
        Reject([]{(void)OSGetAppType();}, "Actual OSInit invented app identity");
        Reject([]{RetireNativeOSDiscBootIdentity();}, "Absent identity retired successfully");

        // Genuine Nod TMD reading plus actual persistent IOS owner, exactly the
        // metadata returned to the production host before it mounts this disc.
        const auto settings = InitializeNativeFilesystemForDisc({argv[1],argv[2],0x1001});
        Check(settings.title_id == 0x0001000052345145ULL && settings.gid == 0xa1b2,
            "Real synthetic TMD title/group did not survive NativeFilesystemBoot");
        Reject([&]{ConfigureNativeOSDiscBootIdentity(settings);},
            "Unmounted medium created a disc boot identity");
        const auto discPath = PathUtf8(argv[1]);
        Check(aurora_dvd_open(discPath.c_str()), "Actual Nod DVD mount failed");
        Check(__DVDGetCoverStatus() == DVD_COVER_CLOSED,
            "Actual mounted DVD did not close its cover");
        auto mismatch=settings;
        mismatch.title_id ^= 1;
        Reject([&]{ConfigureNativeOSDiscBootIdentity(mismatch);},
            "Unrelated TMD title created a mounted disc boot identity");
        Reject([]{(void)OSGetAppType();}, "Rejected setup published partial boot metadata");
        ConfigureNativeOSDiscBootIdentity(settings);
        Check(OSGetAppType() == 0x80, "Verified disc boot lost original u8 DVD app type");
        Reject([&]{ConfigureNativeOSDiscBootIdentity(settings);},
            "Duplicate configuration overwrote a running source boot identity");

        bool getRejected{}, retireRejected{};
        std::thread foreign([&] {
            try {(void)OSGetAppType();} catch (const std::logic_error&) {getRejected=true;}
            try {RetireNativeOSDiscBootIdentity();} catch (const std::logic_error&) {retireRejected=true;}
        });
        foreign.join();
        Check(getRejected && retireRejected && OSGetAppType() == 0x80,
            "Foreign caller consumed or retired the actual boot owner's identity");
        Failures(settings);

        // Original preparation/reset changes the disc drive, not the immutable
        // OS boot application byte. No shutdown success or game reset is inferred.
        __DVDPrepareReset();
        Check(OSGetAppType() == 0x80, "Real DVD reset changed the running application's boot type");
        aurora_dvd_close();
        Check(OSGetAppType() == 0x80, "Real medium removal changed the running application's boot type");
        RetireNativeOSDiscBootIdentity();
        Reject([]{(void)OSGetAppType();}, "Retired boot identity retained source success");
        Reject([]{RetireNativeOSDiscBootIdentity();}, "Duplicate identity retirement returned success");
        ShutdownNativeFilesystem();
        AuroraOSShutdown();
        std::printf("native_os_shutdown_requests: %u checks; real Nod/IOS/OS, no ResetTask or terminal power acceptance\n",checks);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"native_os_shutdown_requests: %s (%u checks)\n",error.what(),checks);
        return 1;
    }
}
