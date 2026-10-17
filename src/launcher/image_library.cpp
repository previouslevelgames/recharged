#include "launcher/image_library.h"

#include "launcher/release_banner.h"
#include "platform/game_release.h"
#include "platform/json.h"
#include "platform/path.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>
#include <iterator>
#include <tuple>

namespace fs = std::filesystem;

namespace mscharged::launcher
{
namespace
{
// Images (of any game) looked at in one scan.
constexpr std::size_t kLibraryFileLimit = 2000;
// Larger cache files are not read.
constexpr std::uintmax_t kMaxCacheFile = 8u << 20;

const char* KindName(DiscProbe::Kind kind)
{
    switch (kind)
    {
    case DiscProbe::Kind::Charged: return "charged";
    case DiscProbe::Kind::OtherGame: return "other";
    case DiscProbe::Kind::NotDisc: return "notdisc";
    default: return "unreadable";
    }
}

void AppendJsonString(std::string& out, std::string_view text)
{
    out += '"';
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (byte < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", unsigned(byte));
            out += escaped;
        }
        else out += c;
    }
    out += '"';
}

std::string Lower(std::string_view text)
{
    std::string lower(text);
    for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// 0 for the USA, 1 Europe, 2 Japan, 3 Korea, 4 anything else.
int RegionRank(const std::string& game_id)
{
    if (game_id.size() != 6) return 4;
    switch (game_id[3])
    {
    case 'E': return 0;
    case 'P': return 1;
    case 'J': return 2;
    case 'K': return 3;
    default: return 4;
    }
}

std::string FileName(const LibraryImage& image) { return PathUtf8(image.path.filename()); }

// Printable ASCII only: a disc header title is Shift JIS on some discs.
bool PlainTitle(const std::string& title)
{
    return !title.empty() && std::all_of(title.begin(), title.end(), [](char c) {
        return static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f;
    });
}
}

// ------------------------------------------------------------------- cache

std::string ImageCache::Key(const fs::path& path)
{
    std::error_code error;
    auto absolute = fs::absolute(path, error);
    if (error) absolute = path;
    return PathUtf8(absolute.lexically_normal());
}

const DiscProbe* ImageCache::Find(const fs::path& path, std::uintmax_t size, std::int64_t modified) const
{
    const auto it = entries_.find(Key(path));
    if (it == entries_.end() || it->second.size != size || it->second.modified != modified) return nullptr;
    return &it->second.probe;
}

const ImageCache::Entry* ImageCache::Last(const fs::path& path) const
{
    const auto it = entries_.find(Key(path));
    return it == entries_.end() ? nullptr : &it->second;
}

void ImageCache::Put(const fs::path& path, std::uintmax_t size, std::int64_t modified, const DiscProbe& probe)
{
    entries_[Key(path)] = {size, modified, probe};
}

void ImageCache::Retain(const std::function<bool(const std::string&)>& keep)
{
    for (auto it = entries_.begin(); it != entries_.end();)
        it = keep(it->first) ? std::next(it) : entries_.erase(it);
}

std::string ImageCache::Serialize() const
{
    std::string out = "{\n  \"version\": 1,\n  \"images\": [";
    bool first = true;
    for (const auto& [key, entry] : entries_)
    {
        out += first ? "\n    {" : ",\n    {";
        first = false;
        out += "\"path\": ";
        AppendJsonString(out, key);
        out += ", \"size\": " + std::to_string(entry.size) + ", \"modified\": " + std::to_string(entry.modified);
        out += ", \"kind\": \"" + std::string(KindName(entry.probe.kind)) + "\"";
        const auto& info = entry.probe.info;
        if (entry.probe.kind == DiscProbe::Kind::Charged || entry.probe.kind == DiscProbe::Kind::OtherGame)
        {
            out += ", \"id\": ";
            AppendJsonString(out, info.game_id);
            out += ", \"title\": ";
            AppendJsonString(out, info.title);
            out += ", \"revision\": " + std::to_string(unsigned(info.revision));
        }
        if (entry.probe.kind == DiscProbe::Kind::Charged)
        {
            out += ", \"format\": ";
            AppendJsonString(out, info.format);
            out += ", \"files\": " + std::to_string(info.file_count);
        }
        if (!entry.probe.error.empty())
        {
            out += ", \"error\": ";
            AppendJsonString(out, entry.probe.error);
        }
        out += "}";
    }
    out += first ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

ImageCache ImageCache::Parse(std::string_view text)
{
    ImageCache cache;
    try
    {
        const auto document = platform::ParseJson(text);
        if (document["version"].Integer() != 1) return {};
        for (const auto& item : document["images"].Items())
        {
            const auto size = item["size"].Integer(), modified = item["modified"].Integer();
            const auto& path = item["path"].Text();
            const auto& kind = item["kind"].Text();
            if (path.empty() || !size || *size < 0 || !modified) continue;
            Entry entry{std::uintmax_t(*size), *modified, {}};
            auto& probe = entry.probe;
            if (kind == "charged" || kind == "other")
            {
                probe.kind = kind == "charged" ? DiscProbe::Kind::Charged : DiscProbe::Kind::OtherGame;
                probe.info.game_id = item["id"].Text();
                probe.info.title = item["title"].Text();
                const auto revision = item["revision"].Integer();
                if (probe.info.game_id.size() != 6 || !revision || *revision < 0 || *revision > 255) continue;
                probe.info.revision = std::uint8_t(*revision);
                probe.info.file_count = 0;
                if (probe.kind == DiscProbe::Kind::Charged)
                {
                    probe.info.format = item["format"].Text();
                    const auto files = item["files"].Integer();
                    if (!files || *files <= 0 || *files > 0xffffffffll) continue;
                    probe.info.file_count = std::uint32_t(*files);
                }
            }
            else if (kind == "notdisc") probe.kind = DiscProbe::Kind::NotDisc;
            else if (kind == "unreadable") probe.kind = DiscProbe::Kind::Unreadable;
            else continue;
            probe.error = item["error"].Text();
            cache.entries_[path] = std::move(entry);
        }
    }
    catch (const std::exception&) { return {}; }
    return cache;
}

ImageCache LoadImageCache(const fs::path& file)
{
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > kMaxCacheFile) return {};
    std::ifstream input(file, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return input ? ImageCache::Parse(text.str()) : ImageCache{};
}

bool SaveImageCache(const ImageCache& cache, const fs::path& file)
{
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    auto temporary = file;
    temporary += ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto text = cache.Serialize();
        output.write(text.data(), std::streamsize(text.size()));
        if (!output) { fs::remove(temporary, error); return false; }
    }
    fs::rename(temporary, file, error);
    if (error) fs::remove(temporary, error);
    return !error;
}

// ------------------------------------------------------------------- scan

ImageState StateOf(const DiscProbe& probe)
{
    if (probe.kind != DiscProbe::Kind::Charged) return ImageState::Unreadable;
    return SupportedRelease(probe.info) ? ImageState::Playable : ImageState::Unsupported;
}

LibraryScan ScanLibrary(const std::vector<LibraryFolder>& folders, ImageCache cache,
                        const std::set<std::string>& banners_known, const std::vector<fs::path>& keep,
                        ScanProgress& progress, const ProbeFunction& probe, const BannerFunction& banner)
{
    LibraryScan scan;
    std::vector<DiscSearchFolder> search;
    std::vector<std::string> unavailable;
    for (const auto& folder : folders)
    {
        std::error_code error;
        const bool available = fs::is_directory(folder.path, error);
        scan.available.push_back(available);
        search.push_back({folder.path, folder.subfolders ? kLibraryDepth : 0});
        if (!available) unavailable.push_back(ImageCache::Key(folder.path));
    }
    auto found = ListDiscImages(search, kLibraryFileLimit + 1, kLibraryEntryLimit);
    if (found.size() > kLibraryFileLimit)
    {
        found.resize(kLibraryFileLimit);
        scan.limited = true;
    }
    progress.found = found.size();
    progress.listing = false;

    std::set<std::string> listed;
    for (const auto& file : found)
    {
        if (progress.cancel)
        {
            scan.cancelled = true;
            break;
        }
        listed.insert(ImageCache::Key(file.path));
        DiscProbe result = probe(file.path);
        cache.Put(file.path, file.size, file.modified, result);
        ++progress.checked;
        if (result.kind == DiscProbe::Kind::OtherGame) { ++scan.other_games; continue; }
        if (result.kind == DiscProbe::Kind::NotDisc) continue;
        const auto& id = result.info.game_id;
        if (result.kind == DiscProbe::Kind::Charged && banner && !banners_known.count(id) && !scan.banners.count(id)
            && !scan.banner_failures.count(id))
        {
            try { scan.banners[id] = banner(file.path, id); }
            catch (const std::exception&) { scan.banner_failures.insert(id); }
        }
        scan.images.push_back({file.path, file.folder, file.size, file.modified, std::move(result)});
    }
    if (!scan.cancelled)
    {
        std::set<std::string> kept;
        for (const auto& path : keep) kept.insert(ImageCache::Key(path));
        cache.Retain([&](const std::string& key) {
            if (listed.count(key) || kept.count(key)) return true;
            const fs::path path = PathFromUtf8(key);
            for (const auto& folder : unavailable)
            {
                const auto relative = path.lexically_relative(PathFromUtf8(folder));
                if (!relative.empty() && *relative.begin() != "..") return true;
            }
            return false;
        });
    }
    scan.cache = std::move(cache);
    return scan;
}

// ------------------------------------------------------------- presenting

std::string ImageTitle(const LibraryImage& image)
{
    if (image.probe.kind != DiscProbe::Kind::Charged) return FileName(image);
    if (const auto* release = FindRelease(image.probe.info)) return release->title;
    // Korea's release is called Mario Power Soccer; its disc header says otherwise.
    if (image.probe.info.game_id == "R4QK01") return "Mario Power Soccer";
    return PlainTitle(image.probe.info.title) ? image.probe.info.title : "Mario Strikers Charged";
}

std::vector<const LibraryImage*> ArrangeImages(const std::vector<LibraryImage>& images, LibrarySort sort,
                                               bool descending)
{
    std::vector<const LibraryImage*> arranged;
    for (const auto& image : images) arranged.push_back(&image);
    const auto name = [](const LibraryImage* image) { return Lower(FileName(*image)); };
    const auto location = [](const LibraryImage* image) { return Lower(PathUtf8(image->path)); };
    const auto release = [&](const LibraryImage* image) {
        const bool charged = image->probe.kind == DiscProbe::Kind::Charged;
        return std::make_tuple(charged ? 0 : 1, charged ? RegionRank(image->probe.info.game_id) : 0,
                               charged ? image->probe.info.revision : 0, name(image), location(image));
    };
    const auto less = [&](const LibraryImage* a, const LibraryImage* b) {
        switch (sort)
        {
        case LibrarySort::Release: return release(a) < release(b);
        case LibrarySort::Name: return std::make_tuple(name(a), location(a)) < std::make_tuple(name(b), location(b));
        case LibrarySort::Format:
        {
            const auto key = [&](const LibraryImage* image) {
                return std::make_tuple(image->probe.kind == DiscProbe::Kind::Charged ? 0 : 1, image->probe.info.format,
                                       release(image));
            };
            return key(a) < key(b);
        }
        default: return std::make_tuple(a->size, name(a), location(a)) < std::make_tuple(b->size, name(b), location(b));
        }
    };
    std::stable_sort(arranged.begin(), arranged.end(), less);
    if (descending) std::reverse(arranged.begin(), arranged.end());
    return arranged;
}

std::vector<const LibraryImage*> SameRelease(const std::vector<LibraryImage>& images, const std::string& game_id,
                                             std::uint8_t revision, const fs::path& except)
{
    std::vector<const LibraryImage*> copies;
    const auto skip = ImageCache::Key(except);
    for (const auto& image : images)
        if (image.probe.kind == DiscProbe::Kind::Charged && image.probe.info.game_id == game_id
            && image.probe.info.revision == revision && ImageCache::Key(image.path) != skip)
            copies.push_back(&image);
    return copies;
}

std::vector<LibraryFolder> FoldersOfImages(const std::vector<fs::path>& images)
{
    std::vector<LibraryFolder> folders;
    std::set<std::string> seen;
    for (const auto& image : images)
    {
        const auto folder = image.parent_path();
        if (!folder.empty() && seen.insert(ImageCache::Key(folder)).second) folders.push_back({folder, false});
    }
    // A folder inside another one (up to kLibraryDepth levels down) is listed
    // by the outer folder with its subfolders.
    const auto depth_below = [](const LibraryFolder& inner, const LibraryFolder& outer) {
        const auto relative =
            PathFromUtf8(ImageCache::Key(inner.path)).lexically_relative(PathFromUtf8(ImageCache::Key(outer.path)));
        if (relative.empty() || *relative.begin() == ".." || relative == ".") return 0;
        return int(std::distance(relative.begin(), relative.end()));
    };
    std::vector<LibraryFolder> merged;
    for (const auto& folder : folders)
    {
        bool inside = false;
        for (const auto& outer : folders)
        {
            const int depth = depth_below(folder, outer);
            inside = inside || (depth > 0 && depth <= kLibraryDepth);
        }
        if (inside) continue;
        LibraryFolder kept = folder;
        for (const auto& inner : folders)
        {
            const int depth = depth_below(inner, folder);
            kept.subfolders = kept.subfolders || (depth > 0 && depth <= kLibraryDepth);
        }
        merged.push_back(kept);
    }
    return merged;
}

bool InFolder(const fs::path& image, const LibraryFolder& folder)
{
    // Absolute and normalised, without a trailing separator ("/games/" is "/games").
    const auto normal = [](const fs::path& path) {
        auto key = PathFromUtf8(ImageCache::Key(path));
        if (key.filename().empty() && key.has_relative_path()) key = key.parent_path();
        return key;
    };
    const auto relative = normal(image.parent_path()).lexically_relative(normal(folder.path));
    if (relative.empty() || *relative.begin() == "..") return false;
    if (relative == ".") return true;
    const auto depth = std::distance(relative.begin(), relative.end());
    return folder.subfolders && depth <= kLibraryDepth;
}

std::string MissingImageReason(const fs::path& path)
{
    std::error_code error;
    if (fs::exists(path, error)) return {};
    const auto folder = path.parent_path();
    if (!folder.empty() && !fs::is_directory(folder, error))
        return "Its folder " + PathUtf8(folder)
            + " is not there. If it is on a drive or memory card, connect it and check again.";
    return PathUtf8(path.filename()) + " is no longer in " + PathUtf8(folder)
        + ". It may have been moved, renamed or deleted.";
}

std::string FileSizeText(std::uintmax_t bytes)
{
    char text[32];
    const double gigabytes = double(bytes) / double(1ull << 30);
    if (gigabytes >= 1.0) std::snprintf(text, sizeof text, "%.2f GB", gigabytes);
    else std::snprintf(text, sizeof text, "%.0f MB", double(bytes) / double(1ull << 20));
    return text;
}
}
