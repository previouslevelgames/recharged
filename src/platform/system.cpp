#include "platform/system.h"
#include <revolution/sc.h>

#include <cstring>
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
    bool simpleAddressSupplied = false;
    std::optional<std::uint32_t> simpleAddressId;
    bool idleModeSupplied = false;
    bool idleModePresent = false;
    std::size_t idleModeBytes = 0;
    SCIdleModeInfo idleMode{};
    std::thread::id owner;
};
SystemDevice device;
static_assert(sizeof(SCIdleModeInfo) == 2 && alignof(SCIdleModeInfo) == 1);
static_assert(offsetof(SCIdleModeInfo, wc24) == 0 && offsetof(SCIdleModeInfo, slotLight) == 1);

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

void ConfigureNativeSystemSimpleAddress(std::optional<std::uint32_t> id)
{
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty)
        throw std::logic_error("Stage native system settings before supplying the simple-address record");
    RequireOwner();
    if (device.phase != Phase::Staged)
        throw std::logic_error("Supply the native simple-address record before original SCInit");
    // Every bit of the source's u32 record is retained, including its explicit
    // zero/FF country encodings. Game source interprets those encodings.
    device.simpleAddressId = id;
    device.simpleAddressSupplied = true;
}

void ConfigureNativeSystemIdleMode(const void* record, std::size_t bytes)
{
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty)
        throw std::logic_error("Stage native system settings before supplying the idle-mode record");
    RequireOwner();
    if (device.phase != Phase::Staged)
        throw std::logic_error("Supply the native idle-mode record before original SCInit");
    if (!record && bytes != 0)
        throw std::invalid_argument("An absent native idle-mode record must have zero length");
    device.idleModeSupplied = true;
    device.idleModePresent = record != nullptr;
    device.idleModeBytes = bytes;
    device.idleMode = {};
    if (record && bytes == sizeof(SCIdleModeInfo))
        std::memcpy(&device.idleMode, record, sizeof(SCIdleModeInfo));
}

void ShutdownNativeSystemSettings()
{
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty) return;
    RequireOwner();
    device.phase = Phase::Empty;
    device.settings = {};
    device.simpleAddressSupplied = false;
    device.simpleAddressId.reset();
    device.idleModeSupplied = false;
    device.idleModePresent = false;
    device.idleModeBytes = 0;
    device.idleMode = {};
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

extern "C" std::uint32_t SCGetSimpleAddressID()
{
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty || !device.simpleAddressSupplied)
        throw std::logic_error("Native simple-address record was not supplied before original source query");
    // Original scapi.c rejects absent/invalid IPL.SADR and returns FFFFFFFF.
    // SCGetSimpleAddressData's region-zero name clearing leaves the ID intact.
    if (!device.simpleAddressId) return 0xFFFFFFFFu;
    const auto id = *device.simpleAddressId;
    const auto country = id & 0xFF000000u;
    const auto region = id & 0x00FF0000u;
    if (id == 0xFFFFFFFFu || country == 0 || country == 0xFF000000u
        || region == 0x00FF0000u) return 0xFFFFFFFFu;
    return id;
}

extern "C" void SCGetIdleMode(SCIdleModeInfo* mode)
{
    // Original SCFindByteArrayItem is a no-op for a null destination.
    if (!mode) return;
    std::lock_guard lock(device.mutex);
    if (device.phase == Phase::Empty || !device.idleModeSupplied)
        throw std::logic_error("Native idle-mode record was not supplied before original source query");
    // scapi.c requests exactly sizeof(SCIdleModeInfo). scsystem.c copies only
    // a present array of that length; absent/malformed records do not write.
    // OSShutdownSystem supplies its own zero default before this request.
    if (device.idleModePresent && device.idleModeBytes == sizeof(SCIdleModeInfo))
        std::memcpy(mode, &device.idleMode, sizeof(SCIdleModeInfo));
}
