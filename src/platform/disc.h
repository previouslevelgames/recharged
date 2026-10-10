#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mscharged
{
struct DiscInfo
{
    std::string format;
    std::string game_id;
    std::string title;
    uint8_t revision;
    uint32_t file_count;
};

// The port runs the USA release, the one the decompilation reconstructs.
inline bool SupportedRelease(const DiscInfo& info) { return info.game_id == "R4QE01"; }

// "USA", "Europe", "Japan", "Korea" from the disc ID's region letter.
const char* DiscRegionName(const std::string& game_id);

// Opens the image read-only and checks the Charged header and data partition.
// This is a metadata/readability check, not a full-disc integrity check.
DiscInfo InspectDisc(const std::filesystem::path& path);

// Actual data-partition TMD title identity for native ES/storage services.
// Does not derive a title from the disc ID or authenticate the TMD signature.
std::uint64_t ReadDiscTitleId(const std::filesystem::path& path);

// Actual BE group identity in the same data-partition TMD header. No UID can
// be derived from this metadata; a native IOS process owns that separately.
std::uint16_t ReadDiscTitleGroupId(const std::filesystem::path& path);
}
