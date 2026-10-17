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
// Entries read per folder by default: bounds a crowded Downloads folder.
constexpr std::size_t kFolderEntryLimit = 512;

// A folder's entries without hidden ones, sorted by name; none when it
// cannot be read.
std::vector<fs::directory_entry> Entries(const fs::path& folder, std::size_t limit = kFolderEntryLimit)
{
    std::vector<fs::directory_entry> entries;
    std::error_code error;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::directory_iterator() && entries.size() < limit; it.increment(error))
    {
        // native(): a name the narrow encoding cannot hold (Windows) must not throw.
        const auto name = it->path().filename().native();
        if (!name.empty() && name[0] != fs::path::value_type('.')) entries.push_back(*it);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
    return entries;
}

// Whether the extension is `wanted` (lower case), in any case.
bool HasExtension(const fs::path& path, const char* wanted)
{
    const auto extension = path.extension().native();
    std::size_t n = 0;
    for (; wanted[n]; ++n)
    {
        if (n >= extension.size()) return false;
        auto c = extension[n];
        if (c >= 'A' && c <= 'Z') c = fs::path::value_type(c - 'A' + 'a');
        if (c != fs::path::value_type(wanted[n])) return false;
    }
    return n == extension.size();
}

bool IsDiscImageName(const fs::path& path)
{
    return HasExtension(path, ".iso") || HasExtension(path, ".rvz") || HasExtension(path, ".wbfs");
}

struct ScanContext
{
    std::size_t limit;
    std::size_t entry_limit;
    std::size_t folder;
    std::vector<FoundImage>& images;
    std::set<fs::path>& seen;
};

void Scan(const fs::path& folder, int depth, ScanContext& context)
{
    std::vector<fs::path> subfolders;
    for (const auto& entry : Entries(folder, context.entry_limit))
    {
        if (context.images.size() >= context.limit) return;
        std::error_code error;
        if (entry.is_directory(error))
        {
            if (depth > 0) subfolders.push_back(entry.path());
            continue;
        }
        if (!IsDiscImageName(entry.path()) || !entry.is_regular_file(error)) continue;
        const auto size = DiscImageSize(entry.path(), error);
        if (error || size < kMinimumImageSize) continue;
        const auto modified = entry.last_write_time(error);
        auto identity = fs::weakly_canonical(entry.path(), error);
        if (error) identity = entry.path();
        if (context.seen.insert(identity).second)
            context.images.push_back({entry.path(), context.folder, size,
                                      std::int64_t(modified.time_since_epoch().count())});
    }
    for (const auto& subfolder : subfolders) Scan(subfolder, depth - 1, context);
}
}

std::uintmax_t DiscImageSize(const fs::path& path, std::error_code& error)
{
    const auto size = fs::file_size(path, error);
    if (error || !HasExtension(path, ".wbfs")) return size;
    // A WBFS split into parts continues in name.wbf1, name.wbf2, ... (the
    // extension's case kept), which the disc reader reads as one image.
    auto extension = path.extension().string();
    extension.pop_back();
    std::uintmax_t total = size;
    for (int index = 1; index < 100; ++index)
    {
        auto part = path;
        part.replace_extension(extension + std::to_string(index));
        std::error_code part_error;
        const auto part_size = fs::file_size(part, part_error);
        if (part_error) break;
        total += part_size;
    }
    return total;
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

std::vector<FoundImage> ListDiscImages(const std::vector<DiscSearchFolder>& folders, std::size_t limit,
                                       std::size_t entry_limit)
{
    std::vector<FoundImage> images;
    std::set<fs::path> seen;
    for (std::size_t n = 0; n < folders.size() && images.size() < limit; ++n)
    {
        ScanContext context{limit, entry_limit, n, images, seen};
        Scan(folders[n].path, folders[n].depth, context);
    }
    return images;
}

std::vector<fs::path> FindDiscImages(const std::vector<DiscSearchFolder>& folders, std::size_t limit)
{
    std::vector<fs::path> images;
    for (auto& image : ListDiscImages(folders, limit)) images.push_back(std::move(image.path));
    return images;
}

DiscSearchResult PickDisc(const std::vector<fs::path>& images,
                          const std::function<DiscInfo(const fs::path&)>& inspect,
                          const std::function<bool(const DiscInfo&)>& playable)
{
    DiscSearchResult result;
    for (const auto& image : images)
    {
        DiscInfo info;
        try { info = inspect(image); }
        catch (const std::exception&) { continue; }
        result.charged.push_back(image);
        if (playable(info))
        {
            if (result.disc.empty()) result.disc = image;
            ++result.supported;
        }
        else if (result.other.empty())
        {
            result.other = image;
            result.other_id = info.game_id;
            result.other_revision = info.revision;
        }
    }
    return result;
}
}
