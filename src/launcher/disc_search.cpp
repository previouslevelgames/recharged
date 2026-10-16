#include "launcher/disc_search.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace mscharged::launcher
{
namespace
{
// Smaller files are not Wii disc images (an RVZ of the game is over 1 GB).
constexpr std::uintmax_t kMinimumImageSize = 32ull << 20;
// Entries read per folder: bounds a crowded Downloads folder.
constexpr std::size_t kFolderEntryLimit = 512;

// A folder's entries without hidden ones, sorted by name; none when it
// cannot be read.
std::vector<fs::directory_entry> Entries(const fs::path& folder)
{
    std::vector<fs::directory_entry> entries;
    std::error_code error;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::directory_iterator() && entries.size() < kFolderEntryLimit; it.increment(error))
    {
        // native(): a name the narrow encoding cannot hold (Windows) must not throw.
        const auto name = it->path().filename().native();
        if (!name.empty() && name[0] != fs::path::value_type('.')) entries.push_back(*it);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
    return entries;
}

bool IsDiscImageName(const fs::path& path)
{
    const auto extension = path.extension().native();
    const auto is = [&](const char* wanted) {
        std::size_t n = 0;
        for (; wanted[n]; ++n)
        {
            if (n >= extension.size()) return false;
            auto c = extension[n];
            if (c >= 'A' && c <= 'Z') c = fs::path::value_type(c - 'A' + 'a');
            if (c != fs::path::value_type(wanted[n])) return false;
        }
        return n == extension.size();
    };
    return is(".iso") || is(".rvz");
}

void Scan(const fs::path& folder, int depth, std::size_t limit, std::vector<fs::path>& images, std::set<fs::path>& seen)
{
    std::vector<fs::path> subfolders;
    for (const auto& entry : Entries(folder))
    {
        if (images.size() >= limit) return;
        std::error_code error;
        if (entry.is_directory(error))
        {
            if (depth > 0) subfolders.push_back(entry.path());
            continue;
        }
        if (!IsDiscImageName(entry.path()) || !entry.is_regular_file(error)) continue;
        const auto size = entry.file_size(error);
        if (error || size < kMinimumImageSize) continue;
        auto identity = fs::weakly_canonical(entry.path(), error);
        if (error) identity = entry.path();
        if (seen.insert(identity).second) images.push_back(entry.path());
    }
    for (const auto& subfolder : subfolders) Scan(subfolder, depth - 1, limit, images, seen);
}
}

std::vector<DiscSearchFolder> DiscSearchFolders(const fs::path& settings_folder, const fs::path& home,
                                                const fs::path& downloads, const std::vector<fs::path>& drives)
{
    std::vector<DiscSearchFolder> folders;
    const auto add = [&](const fs::path& path, int depth) {
        if (!path.empty()) folders.push_back({path, depth});
    };
    if (!settings_folder.empty())
    {
        add(settings_folder / "game", 1);
        add(settings_folder, 0);
    }
    if (!home.empty())
    {
        add(home / "Games", 1);
        add(home / "Emulation" / "roms" / "wii", 1);
    }
    add(downloads, 1);
    for (const auto& drive : drives)
    {
        add(drive / "Emulation" / "roms" / "wii", 1);
        add(drive, 0);
    }
    return folders;
}

std::vector<fs::path> RemovableDrives(const fs::path& media_root, const std::string& user)
{
    std::vector<fs::path> drives;
    for (const auto& entry : Entries(media_root))
    {
        std::error_code error;
        if (!entry.is_directory(error)) continue;
        if (user.empty() || entry.path().filename() != user)
        {
            drives.push_back(entry.path());
            continue;
        }
        for (const auto& drive : Entries(entry.path()))
            if (drive.is_directory(error)) drives.push_back(drive.path());
    }
    return drives;
}

std::vector<fs::path> FindDiscImages(const std::vector<DiscSearchFolder>& folders, std::size_t limit)
{
    std::vector<fs::path> images;
    std::set<fs::path> seen;
    for (const auto& folder : folders)
        if (images.size() < limit) Scan(folder.path, folder.depth, limit, images, seen);
    return images;
}

DiscSearchResult PickDisc(const std::vector<fs::path>& images,
                          const std::function<DiscInfo(const fs::path&)>& inspect)
{
    DiscSearchResult result;
    for (const auto& image : images)
    {
        DiscInfo info;
        try { info = inspect(image); }
        catch (const std::exception&) { continue; }
        if (SupportedRelease(info))
        {
            if (result.disc.empty()) result.disc = image;
            ++result.supported;
        }
        else if (result.other.empty())
        {
            result.other = image;
            result.other_id = info.game_id;
        }
    }
    return result;
}
}
