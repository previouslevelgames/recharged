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
