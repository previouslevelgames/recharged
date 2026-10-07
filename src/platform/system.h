#pragma once
#include <cstdint>
#include <optional>

namespace mscharged
{
// Explicit virtual Wii system records, supplied before original construction.
// Values follow the original SC enums; they do not select a game language or
// render mode. Those branches remain in the reconstructed game source.
struct NativeSystemSettings
{
    std::uint8_t language;
    std::uint8_t progressive_mode;
    std::uint8_t eurgb60_mode;
    std::uint8_t aspect_ratio;
    // Wii SC_SND_STEREO. Existing diagnostics explicitly default to stereo;
    // original movie/audio source still chooses its behavior from SC queries.
    std::uint8_t sound_mode = 1;
};

// Stage a validated settings snapshot on the host owner before game entry.
// Source SCInit publishes its initialized status. Queries may read the staged
// records earlier, as original main queries SCGetLanguage before glplatStartup.
void ConfigureNativeSystemSettings(const NativeSystemSettings& settings);
// A separate explicit virtual-Wii IPL.SADR ID record. Its country, region and
// city fields follow the original SC bit positions. No locale/disc inference
// or implicit zero record is supplied; stage before original SCInit.
// nullopt explicitly supplies an absent record. Invalid source encodings are
// retained in the backing record; SCGetSimpleAddressID returns FFFFFFFF for
// them according to the original SC contract, rather than choosing a country.
void ConfigureNativeSystemSimpleAddress(std::optional<std::uint32_t> id);
// Retire after original users have stopped; a later session configures anew.
void ShutdownNativeSystemSettings();

// Compatibility for existing selected-source startup/scene diagnostics only.
// Explicitly stages language plus interlaced, EURGB50, standard-aspect records.
// Production original entry must supply the entire user settings snapshot.
void SetStartupSystemLanguage(std::uint8_t language);
}
