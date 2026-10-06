#include "platform/system.h"
#include <revolution/sc.h>

#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
enum class Phase { Empty, Staged, Initialized };
struct SystemDevice
{
    std::mutex mutex;
    Phase phase = Phase::Empty;
    mscharged::NativeSystemSettings settings{};
    std::thread::id owner;
};
SystemDevice device;

void RequireOwner()
{
    if (device.owner != std::this_thread::get_id())
        throw std::logic_error("Native system settings lifecycle requires its configured owner");
}

mscharged::NativeSystemSettings ReadSettings()
{
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty)
        throw std::logic_error("Native system settings were not supplied before original source query");
    return device.settings;
}
}

namespace mscharged
{
void ConfigureNativeSystemSettings(const NativeSystemSettings& settings)
{
    if (settings.language >= SC_LANG_MAX || settings.progressive_mode > SC_PROGRESSIVE
        || settings.eurgb60_mode > SC_EURGB_60_HZ || settings.aspect_ratio > SC_ASPECT_WIDE
        || settings.sound_mode > SC_SND_SURROUND)
        throw std::invalid_argument("Native system settings contain unsupported Wii SC values");
    std::lock_guard lock(device.mutex);
    if (device.phase != Phase::Empty) RequireOwner();
    if (device.phase == Phase::Initialized)
        throw std::logic_error("Retire native system settings before configuring a new original session");
    device.settings = settings;
    device.owner = std::this_thread::get_id();
    device.phase = Phase::Staged;
}

void ShutdownNativeSystemSettings()
{
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty) return;
    RequireOwner();
    device.phase = Phase::Empty;
    device.settings = {};
    device.owner = {};
}

void SetStartupSystemLanguage(std::uint8_t language)
{
    ConfigureNativeSystemSettings({language, SC_INTERLACED, SC_EURGB_50_HZ, SC_ASPECT_STD});
}
}

extern "C" void SCInit()
{
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty)
        throw std::logic_error("Original SCInit requires explicit native system settings");
    RequireOwner();
    // The host supplied and validated the complete bounded backing records.
    // No NAND worker/read is pending for this native read-only endpoint.
    device.phase = Phase::Initialized;
}

extern "C" std::uint32_t SCCheckStatus()
{
    std::lock_guard lock(device.mutex);
    switch (device.phase)
    {
    case Phase::Empty: return SC_STATUS_FATAL;
    case Phase::Staged: return SC_STATUS_BUSY;
    case Phase::Initialized: return SC_STATUS_OK;
    }
    throw std::logic_error("Invalid native system settings state");
}

extern "C" std::uint8_t SCGetLanguage() { return ReadSettings().language; }
extern "C" std::uint8_t SCGetProgressiveMode() { return ReadSettings().progressive_mode; }
extern "C" std::uint8_t SCGetEuRgb60Mode() { return ReadSettings().eurgb60_mode; }
extern "C" std::uint8_t SCGetAspectRatio() { return ReadSettings().aspect_ratio; }
extern "C" std::uint8_t SCGetSoundMode() { return ReadSettings().sound_mode; }
