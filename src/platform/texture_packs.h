#pragma once

#include "platform/zip_archive.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged::platform
{
// HD texture packs in Dolphin's format: folders or .zip files of PNG/DDS
// images named tex1_<width>x<height>[_m]_<hash>[_<palette hash>]_<format>
// (plus <name>_mipN levels). The game host hands their images to Aurora,
// which swaps them in for the game's textures with the same hash.
struct TexturePack
{
    std::string name;             // its file or folder name in the packs folder ("R4Q", "R4Q.zip")
    std::filesystem::path path;
    bool archive = false;         // a .zip file
    std::size_t textures = 0;     // replacement images (extra mip levels not counted)
    std::string error;            // why it cannot be read, when it cannot
};

// The packs in `folder`: its subfolders and .zip files that hold replacement
// images, by name. A folder that holds images itself, or is named after the
// game (R4Q, R4QE01, ...), is one pack. Folders named after another game's ID
// (a Dolphin Load/Textures folder) and hidden entries (".name", such as a
// pack still being copied in) are left out.
std::vector<TexturePack> FindTexturePacks(const std::filesystem::path& folder);

// One pack: a folder or .zip file that holds replacement images, or nothing.
std::optional<TexturePack> ProbeTexturePack(const std::filesystem::path& path);

// `packs` in the player's order: the names in `order` first, the others after
// them by name.
std::vector<TexturePack> OrderTexturePacks(std::vector<TexturePack> packs, const std::vector<std::string>& order);

// Names kept in a settings value ("A | B"): split and joined.
std::vector<std::string> SplitPackNames(std::string_view value);
std::string JoinPackNames(const std::vector<std::string>& names);
// A name that fits in that list: no '|', line break or NUL.
bool StorablePackName(std::string_view name);

// A replacement image's file name (tex1_..., .png or .dds), not a mip level.
bool IsReplacementImageName(std::string_view filename);

// The images of one pack, read from any thread.
class TexturePackFiles
{
public:
    // The pack's images for the game `game_id` (such as R4QE01): images in a
    // folder named after another release of the game (R4QP01 for R4QE01) are
    // left out. Throws std::runtime_error for an unreadable zip.
    TexturePackFiles(const TexturePack& pack, std::string_view game_id);

    // '/'-separated paths in the pack, sorted as Dolphin sorts them (the
    // first image of a name wins, .dds before .png).
    const std::vector<std::string>& Images() const { return images_; }
    // The file at `path` (an image or one of its _mipN levels); false when
    // it does not exist or cannot be read.
    bool Read(std::string_view path, std::vector<std::uint8_t>& out) const;

private:
    std::filesystem::path root_;
    std::unique_ptr<ZipArchive> archive_;
    std::vector<std::string> images_;
};
}
