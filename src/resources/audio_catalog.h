#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mscharged::resources
{
struct AudioBankGroupRecord
{
    std::uint32_t id, hash;
    std::vector<std::uint32_t> slots;
};
struct AudioBankSlotRecord
{
    std::uint32_t id, hash, group;
    bool streaming;
};
struct AudioBankNameRecord { std::uint32_t id; std::string name; };
struct AudioBankCatalog
{
    using Handle = std::shared_ptr<const AudioBankCatalog>;
    std::vector<AudioBankGroupRecord> groups;
    std::vector<AudioBankSlotRecord> slots;
    std::vector<AudioBankNameRecord> names;
};
using AudioCueKey = std::array<std::uint32_t, 4>;
struct AudioCueCatalog
{
    using Handle = std::shared_ptr<const AudioCueCatalog>;
    std::map<AudioCueKey, std::uint32_t> cues;
    // SoundMap::FindCue returns 0xffff for an absent exact four-part key.
    std::uint32_t Find(const AudioCueKey&) const;
};
// Decode the bank-table section of nlxgs.bun and SoundMap section of a resbun.
// Relocate serialized 32-bit addresses to checked indices, never host pointers.
// Other sections remain unselected. These readers do not initialize audio,
// publish loaded bank slots, choose voices or report successful sound playback.
AudioBankCatalog::Handle ReadAudioBankCatalog(Bytes);
AudioCueCatalog::Handle ReadAudioCueCatalog(Bytes);
}
