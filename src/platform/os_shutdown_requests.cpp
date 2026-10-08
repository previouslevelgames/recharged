#include "platform/os_shutdown_requests.h"
#include "platform/os_shutdown_requests_abi.h"
#include "platform/interrupts.h"

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr std::uint8_t DiscApplication = 0x80;
struct BootIdentity {
    std::mutex mutex;
    bool configured{};
    std::thread::id owner;
    std::uint64_t title{};
    DVDDiskID disc{};
};
BootIdentity& Identity() {
    static BootIdentity identity;
    return identity;
}
void RequireSDK() {
    if (!OSGetArenaLo() || !OSGetArenaHi() || !OSGetPhysicalMemSize())
        throw std::logic_error("native OS disc boot identity needs live SDK arenas");
}
void RequireOwner(const BootIdentity& identity) {
    if (!identity.configured || identity.owner != std::this_thread::get_id())
        throw std::logic_error("native OS disc boot identity is absent, retired or foreign");
    RequireSDK();
}
} // namespace

namespace mscharged::platform {
void ConfigureNativeOSDiscBootIdentity(const NativeFilesystemSettings& boot) {
    if (!NativeInterruptsEnabled() || !NativeInterruptWaitAllowed())
        throw std::logic_error("native OS disc boot setup needs the enabled owner outside hardware guards");
    RequireSDK();
    auto& identity = Identity();
    std::lock_guard lock(identity.mutex);
    if (identity.configured)
        throw std::logic_error("native OS disc boot identity is already configured");

    // NativeFilesystemBoot already verified the Charged Wii image and read its
    // actual data-partition TMD. Reuse that result; do not reopen the image or
    // infer a channel/IPL identity from the host account or saved preferences.
    const auto* disc = DVDGetCurrentDiskID();
    if (__DVDGetCoverStatus() != DVD_COVER_CLOSED ||
        DVDGetDriveStatus() != DVD_STATE_END || !disc ||
        std::memcmp(disc->gameName, "R4Q", 3) ||
        std::memcmp(disc->company, "01", 2))
        throw std::logic_error("native OS disc boot identity needs the actual mounted Charged disc");
    std::uint32_t game{};
    for (unsigned i=0; i<sizeof(disc->gameName); ++i)
        game = (game << 8) | static_cast<unsigned char>(disc->gameName[i]);
    if (boot.title_id != ((std::uint64_t{0x00010000} << 32) | game))
        throw std::logic_error("native OS disc boot TMD identity does not match the mounted disc application");

    identity.disc = *disc;
    identity.title = boot.title_id;
    identity.owner = std::this_thread::get_id();
    identity.configured = true;
}

void RetireNativeOSDiscBootIdentity() {
    auto& identity = Identity();
    std::lock_guard lock(identity.mutex);
    RequireOwner(identity);
    identity.configured = false;
    identity.owner = {};
    identity.title = 0;
    identity.disc = {};
}
} // namespace mscharged::platform

extern "C" std::uint8_t OSGetAppType(void) {
    auto& identity = Identity();
    std::lock_guard lock(identity.mutex);
    RequireOwner(identity);
    // Original OS.c reads a u8 at physical0x3184 outside IPL. This explicit
    // native disc boot supplies that same DVD application value, not IPL or a
    // channel launch. It remains immutable for the running source instance.
    return DiscApplication;
}

extern "C" void __OSReboot(std::uint32_t resetCode, std::uint32_t bootDol) {
    throw std::logic_error("native OS DOL reboot is unsupported (resetCode=" +
        std::to_string(resetCode) + ", bootDol=" + std::to_string(bootDol) + ")");
}
extern "C" void __OSLaunchMenu(void) {
    throw std::logic_error("native OS Wii System Menu launch is unsupported");
}
extern "C" void __OSRelaunchTitle(void) {
    throw std::logic_error("native OS Wii title relaunch is unsupported");
}
extern "C" void __VISetRGBModeImm(void) {
    throw std::logic_error("native VI immediate System Menu RGB mode is unqualified");
}
extern "C" std::int32_t __PADDisableRecalibration(std::int32_t disable) {
    throw std::logic_error("native GameCube pad shutdown recalibration is unqualified (disable=" +
        std::to_string(disable) + ")");
}
