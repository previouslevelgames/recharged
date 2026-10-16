#pragma once

#include "platform/disc.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mscharged::launcher
{
// A folder searched for disc images: its own files, and with depth 1 also
// the files of its subfolders.
struct DiscSearchFolder
{
    std::filesystem::path path;
    int depth = 1;
};

// Where the launcher looks when no disc is chosen, best first: the settings
// folder's game/ (where the guides put the image), the settings folder, then
// Games, EmuDeck's Emulation/roms/wii and Downloads in the home folder, then
// Emulation/roms/wii and the top folder of each removable drive. Empty paths
// are left out.
std::vector<DiscSearchFolder> DiscSearchFolders(const std::filesystem::path& settings_folder,
                                                const std::filesystem::path& home,
                                                const std::filesystem::path& downloads,
                                                const std::vector<std::filesystem::path>& drives);

// Mounted removable drives below `media_root` (/run/media on Linux): its
// folders, and for the user's own folder (/run/media/<user>) the drives in it.
std::vector<std::filesystem::path> RemovableDrives(const std::filesystem::path& media_root, const std::string& user);

// ISO and RVZ files of at least 32 MiB in these folders: folder by folder,
// by name within a folder, at most `limit`. Hidden entries and unreadable
// folders are skipped, and each file is listed once.
std::vector<std::filesystem::path> FindDiscImages(const std::vector<DiscSearchFolder>& folders,
                                                  std::size_t limit = 16);

struct DiscSearchResult
{
    std::filesystem::path disc;  // the first supported image; empty when none
    std::size_t supported = 0;   // how many supported images were found
    std::filesystem::path other; // the first Mario Strikers Charged image of another region
    std::string other_id;        // its game ID
};

// Checks the images in order and picks the first supported release.
// `inspect` throws for anything that is not Mario Strikers Charged.
DiscSearchResult PickDisc(const std::vector<std::filesystem::path>& images,
                          const std::function<DiscInfo(const std::filesystem::path&)>& inspect);
}
