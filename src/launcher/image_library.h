#pragma once

#include "launcher/disc_search.h"
#include "platform/disc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The launcher's image library (#65): the Mario Strikers Charged images in
// the player's game folders, checked in the background and remembered
// between starts. Which image the game starts with ([game] disc) is chosen
// separately; nothing here changes it.
namespace mscharged::launcher
{
// A folder of the library ([library] folderN). With subfolders it also lists
// the images in its subfolders, kLibraryDepth levels down.
struct LibraryFolder
{
    std::filesystem::path path;
    bool subfolders = true;
};
constexpr int kLibraryDepth = 4;
// Images listed at most, and folder entries read per folder.
constexpr std::size_t kLibraryImageLimit = 400;
constexpr std::size_t kLibraryEntryLimit = 4096;

// What earlier checks found, by file: what a chosen image that has gone was.
// The list itself always comes from a fresh scan.
class ImageCache
{
public:
    struct Entry
    {
        std::uintmax_t size = 0;
        std::int64_t modified = 0;
        DiscProbe probe;
    };

    // The entry for this file, if it still has this size and time.
    const DiscProbe* Find(const std::filesystem::path& path, std::uintmax_t size, std::int64_t modified) const;
    // The last entry for this file, whatever its size and time now (what a
    // file that has gone was).
    const Entry* Last(const std::filesystem::path& path) const;
    void Put(const std::filesystem::path& path, std::uintmax_t size, std::int64_t modified, const DiscProbe& probe);
    // Keeps the entries whose path (as Key gives it) `keep` accepts.
    void Retain(const std::function<bool(const std::string& key)>& keep);
    std::size_t size() const { return entries_.size(); }

    // The cache file's JSON text, and the cache it holds (empty for text it
    // cannot read: the cache is only a shortcut).
    std::string Serialize() const;
    static ImageCache Parse(std::string_view text);
    // The key of a path: absolute and normalised, in UTF-8.
    static std::string Key(const std::filesystem::path& path);

private:
    std::map<std::string, Entry> entries_;
};

// Reading never fails (a missing or damaged file is an empty cache); saving
// writes a temporary file and renames it, and returns false on failure.
ImageCache LoadImageCache(const std::filesystem::path& file);
bool SaveImageCache(const ImageCache& cache, const std::filesystem::path& file);

// One image of the library: Mario Strikers Charged (probe.kind Charged), or a
// disc image that cannot be read (Unreadable). Other games are left out.
struct LibraryImage
{
    std::filesystem::path path;
    std::size_t folder = 0; // the library folder it was found in
    std::uintmax_t size = 0;
    std::int64_t modified = 0;
    DiscProbe probe;
};

enum class ImageState
{
    Playable,    // a release this build runs: it can be chosen for Play
    Unsupported, // Mario Strikers Charged in a release this build does not run yet
    Unreadable,  // the file cannot be read as a disc image
};
ImageState StateOf(const DiscProbe& probe);
inline ImageState StateOf(const LibraryImage& image) { return StateOf(image.probe); }

// The progress of a scan, shared with the launcher while it runs.
struct ScanProgress
{
    std::atomic<std::size_t> found{0};   // image files listed so far
    std::atomic<std::size_t> checked{0}; // of those, checked
    std::atomic<bool> listing{true};     // still looking through the folders
    std::atomic<bool> cancel{false};     // set to end the scan early
};

struct LibraryScan
{
    std::vector<LibraryImage> images;     // folder by folder, by name
    std::vector<bool> available;          // for each folder: it exists and can be read
    std::size_t other_games = 0;          // images of other games, not listed
    std::vector<std::filesystem::path> strikers; // of those, Super Mario Strikers (GameCube): the Super Team's disc
    bool limited = false;                 // more than kLibraryImageLimit images
    bool cancelled = false;
    ImageCache cache;                     // the cache, updated with this scan
    // Banners read in this scan (RGBA, release_banner.h), by disc ID.
    std::map<std::string, std::vector<std::uint8_t>> banners;
    std::set<std::string> banner_failures; // disc IDs whose banner could not be read
};

using ProbeFunction = std::function<DiscProbe(const std::filesystem::path&)>;
using BannerFunction =
    std::function<std::vector<std::uint8_t>(const std::filesystem::path&, const std::string& game_id)>;

// Lists and checks the images of these folders, opening every one: the list
// never comes from earlier results. The returned cache records them, and keeps
// the files below folders that are not available now (an unplugged drive) and
// the paths in `keep`. A banner is read for each release whose disc ID is not
// in `banners_known`.
LibraryScan ScanLibrary(const std::vector<LibraryFolder>& folders, ImageCache cache,
                        const std::set<std::string>& banners_known, const std::vector<std::filesystem::path>& keep,
                        ScanProgress& progress, const ProbeFunction& probe = ProbeDisc,
                        const BannerFunction& banner = nullptr);

// Whether a scan of this folder covers this path: an image in the folder
// itself, or with subfolders up to kLibraryDepth levels below it. Compares
// paths only; the file need not exist.
bool InFolder(const std::filesystem::path& image, const LibraryFolder& folder);

// How the library's table can be ordered (its column headers).
enum class LibrarySort
{
    Release, // region, then revision, then file name; unreadable files last
    Name,    // file name
    Format,  // ISO, RVZ or WBFS, then release
    Size,    // file size, smallest first
};
// The images in this order; `descending` reverses it.
std::vector<const LibraryImage*> ArrangeImages(const std::vector<LibraryImage>& images, LibrarySort sort,
                                               bool descending);

// "Mario Strikers Charged Football": the release's title, the disc header's
// for a release Recharged does not know, or the file name of an unreadable image.
std::string ImageTitle(const LibraryImage& image);

// Library images of this release (disc ID and revision) other than `except`.
std::vector<const LibraryImage*> SameRelease(const std::vector<LibraryImage>& images, const std::string& game_id,
                                             std::uint8_t revision, const std::filesystem::path& except);

// The library folders a search found these images in: each image's own
// folder, each once, in order. A folder inside another one is left to the
// outer folder, which then lists its subfolders.
std::vector<LibraryFolder> FoldersOfImages(const std::vector<std::filesystem::path>& images);

// Why an image file cannot be opened any more: its folder (or drive) is not
// there, or the file is gone from its folder. Empty when the file exists.
std::string MissingImageReason(const std::filesystem::path& path);

// "1.4 GB", "820 MB".
std::string FileSizeText(std::uintmax_t bytes);
}
