#include "launcher/disc_search.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

using namespace mscharged;
using namespace mscharged::launcher;
namespace fs = std::filesystem;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// A file of `size` bytes (sparse where the file system allows).
void MakeFile(const fs::path& path, std::uintmax_t size)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('x');
    fs::resize_file(path, size);
}

constexpr std::uintmax_t kImage = 40ull << 20;

std::string Names(const std::vector<fs::path>& paths, const fs::path& root)
{
    std::string text;
    for (const auto& path : paths) text += (text.empty() ? "" : ", ") + fs::relative(path, root).generic_string();
    return text;
}
} // namespace

int main()
{
    const auto root = fs::temp_directory_path() / ("mscharged-disc-search-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        fs::remove_all(root);
        // The order folders are searched in, empty paths left out.
        const auto folders = DiscSearchFolders(root / "port", root / "home", "", {root / "sd"});
        Require(folders.size() == 6, "Six search folders");
        Require(folders[0].path == root / "port" / "game" && folders[0].depth == 1, "The game folder comes first");
        Require(folders[1].path == root / "port" && folders[1].depth == 0, "The settings folder, without subfolders");
        Require(folders[2].path == root / "home" / "Games", "Then ~/Games");
        Require(folders[3].path == root / "home" / "Emulation" / "roms" / "wii", "Then EmuDeck's Wii folder");
        Require(folders[4].path == root / "sd" / "Emulation" / "roms" / "wii", "Then the drive's EmuDeck folder");
        Require(folders[5].path == root / "sd" && folders[5].depth == 0, "Then the drive itself");

        MakeFile(root / "port" / "game" / "b.rvz", kImage);
        MakeFile(root / "port" / "game" / "a.ISO", kImage);
        MakeFile(root / "port" / "game" / "Charged (USA)" / "c.iso", kImage);
        MakeFile(root / "port" / "game" / "small.iso", 1024);
        MakeFile(root / "port" / "game" / "notes.txt", kImage);
        MakeFile(root / "port" / "game" / ".hidden" / "d.iso", kImage);
        MakeFile(root / "port" / "game" / "one" / "two" / "too-deep.iso", kImage);
        MakeFile(root / "port" / "beside.rvz", kImage);
        MakeFile(root / "port" / "logs" / "not-searched.iso", kImage);
        MakeFile(root / "home" / "Games" / "Wii" / "e.rvz", kImage);
        MakeFile(root / "sd" / "Emulation" / "roms" / "wii" / "f.rvz", kImage);
        auto images = FindDiscImages(folders);
        Require(Names(images, root) == "port/game/a.ISO, port/game/b.rvz, port/game/Charged (USA)/c.iso, "
                                       "port/beside.rvz, home/Games/Wii/e.rvz, sd/Emulation/roms/wii/f.rvz",
                "Found " + Names(images, root));
        Require(FindDiscImages(folders, 2).size() == 2, "The limit holds");
        // A folder searched twice (a link to it, or the same drive mounted twice) lists its files once.
        Require(FindDiscImages({{root / "port" / "game", 0}, {root / "port" / "game" / ".", 0}}).size() == 2,
                "Each file is listed once");
        Require(FindDiscImages({{root / "missing", 1}}).empty(), "A missing folder finds nothing");

        // /run/media: drives directly in it, and in the user's own folder.
        fs::create_directories(root / "media" / "deck" / "SD card");
        fs::create_directories(root / "media" / "mmcblk0p1");
        fs::create_directories(root / "media" / ".hidden");
        const auto drives = RemovableDrives(root / "media", "deck");
        Require(Names(drives, root) == "media/deck/SD card, media/mmcblk0p1", "Drives " + Names(drives, root));
        Require(RemovableDrives(root / "no-media", "deck").empty(), "No media folder, no drives");

        // The first supported release wins; another region is remembered for the message.
        const std::map<std::string, std::string> ids{{"eu.rvz", "R4QP01"}, {"us1.rvz", "R4QE01"}, {"us2.iso", "R4QE01"}};
        const auto inspect = [&](const fs::path& path) {
            const auto id = ids.find(path.filename().string());
            if (id == ids.end()) throw std::runtime_error("Not Mario Strikers Charged");
            DiscInfo info{"RVZ", id->second, "Mario Strikers Charged", 1, 100};
            return info;
        };
        auto result = PickDisc({"other.iso", "eu.rvz", "us1.rvz", "us2.iso"}, inspect);
        Require(result.disc == "us1.rvz" && result.supported == 2, "The first USA disc is picked");
        Require(result.other == "eu.rvz" && result.other_id == "R4QP01", "The European disc is remembered");
        result = PickDisc({"other.iso", "eu.rvz"}, inspect);
        Require(result.disc.empty() && result.supported == 0 && result.other == "eu.rvz", "Only another region");
        result = PickDisc({}, inspect);
        Require(result.disc.empty() && result.other.empty(), "Nothing to check");

        fs::remove_all(root);
        std::puts("Disc search checks passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        std::fprintf(stderr, "launcher_disc_search: %s\n", error.what());
        return 1;
    }
}
