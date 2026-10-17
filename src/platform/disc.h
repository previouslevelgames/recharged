#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

// A release in platform/game_release.h whose game module is in this build.
bool SupportedRelease(const DiscInfo& info);

// "USA", "Europe", "Japan", "Korea" from the disc ID's region letter.
const char* DiscRegionName(const std::string& game_id);

// Opens the image read-only and checks the Charged header and data partition.
// This is a metadata/readability check, not a full-disc integrity check.
DiscInfo InspectDisc(const std::filesystem::path& path);

// What InspectDisc finds in a file, without throwing: the launcher's image
// library lists Charged images, leaves other games out and shows the rest as
// unreadable.
struct DiscProbe
{
    enum class Kind
    {
        Charged,    // Mario Strikers Charged with a readable game partition: `info`
        OtherGame,  // a GameCube or Wii image of another game: `info` has its ID and title
        NotDisc,    // no disc image format the reader knows (`error` says so)
        Unreadable, // a disc image that cannot be read, or not as ISO, RVZ or WBFS: `error`
    };
    Kind kind = Kind::Unreadable;
    DiscInfo info{};
    std::string error;
};
DiscProbe ProbeDisc(const std::filesystem::path& path);

// A file of a Charged image's data partition, by its path there (any case,
// e.g. "Art/fe/us_banner.tpl"). Throws std::runtime_error when the image or
// the file cannot be read or the file is larger than `max_size` bytes.
std::vector<std::uint8_t> ReadDiscFile(const std::filesystem::path& disc, const std::string& file,
                                       std::size_t max_size);

// The game's main.dol from the data partition, as its boot header locates it.
std::vector<std::uint8_t> ReadDiscMainDol(const std::filesystem::path& path);

// Actual data-partition TMD title identity for native ES/storage services.
// Does not derive a title from the disc ID or authenticate the TMD signature.
std::uint64_t ReadDiscTitleId(const std::filesystem::path& path);

// Actual BE group identity in the same data-partition TMD header. No UID can
// be derived from this metadata; a native IOS process owns that separately.
std::uint16_t ReadDiscTitleGroupId(const std::filesystem::path& path);
}
