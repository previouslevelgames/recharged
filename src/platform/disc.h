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
    std::string dol_sha1; // SHA-1 of the data partition's main.dol, lower-case hex
};

// main.dol of Mario Strikers Charged USA (R4QE01) revision 1, the only release
// the decompilation reconstructs (mscharged-decomp config/R4QE01/config.yml).
inline constexpr const char* kUsaMainDolSha1 = "e96d2298067b70652752145b4e63644e6e1b1560";

// The port runs exactly the reconstructed release: USA revision 1 with its
// original main.dol.
inline bool SupportedRelease(const DiscInfo& info)
{
    return info.game_id == "R4QE01" && info.revision == 1 && info.dol_sha1 == kUsaMainDolSha1;
}

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
