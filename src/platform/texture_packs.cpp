#include "platform/texture_packs.h"

#include "platform/path.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>

namespace mscharged::platform
{
namespace fs = std::filesystem;

namespace
{
// A folder tree larger than this is not looked through further (a packs
// folder set to a whole drive must not stall the launcher).
constexpr std::size_t kMaxFolderEntries = 400'000;
constexpr std::uint64_t kMaxImageFile = ZipArchive::kMaxEntrySize;

bool EndsWithNoCase(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size()
        && std::equal(suffix.begin(), suffix.end(), text.end() - std::ptrdiff_t(suffix.size()),
                      [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
}

std::string Upper(std::string_view text)
{
    std::string result(text);
    for (auto& c : result) c = char(std::toupper((unsigned char)c));
    return result;
}

bool GameIdShape(std::string_view name)
{
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}

// Mario Strikers Charged's folders: R4Q (every release) and R4QE01, R4QP01, ...
bool ThisGameFolder(std::string_view name)
{
    const auto upper = Upper(name);
    return (upper.size() == 3 || upper.size() == 6) && upper.compare(0, 3, "R4Q") == 0 && GameIdShape(upper);
}

// Another game's folder in a Dolphin Load/Textures folder (GALE01, ...): six
// capitals and digits with a digit among them, not R4Q.
bool OtherGameFolder(std::string_view name)
{
    return name.size() == 6 && GameIdShape(name) && name.compare(0, 3, "R4Q") != 0
        && std::any_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The folder of another release of the game (R4QP01 while R4QE01 plays).
bool OtherReleaseFolder(std::string_view component, std::string_view game_id)
{
    return component.size() == 6 && ThisGameFolder(component) && Upper(component) != Upper(game_id);
}

std::string Generic(const fs::path& path)
{
    auto text = PathUtf8(path);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

// Replacement images below `folder`, as '/'-separated paths relative to it.
std::vector<std::string> FolderImages(const fs::path& folder)
{
    std::vector<std::string> images;
    std::error_code error;
    std::size_t seen = 0;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error))
    {
        if (++seen > kMaxFolderEntries) break;
        std::error_code type_error;
        if (!it->is_regular_file(type_error)) continue;
        if (!IsReplacementImageName(PathUtf8(it->path().filename()))) continue;
        images.push_back(Generic(it->path().lexically_relative(folder)));
    }
    return images;
}

std::vector<std::string> ArchiveImages(const ZipArchive& archive)
{
    std::vector<std::string> images;
    for (const auto& entry : archive.Entries())
    {
        const auto slash = entry.name.rfind('/');
        const std::string_view filename =
            slash == std::string::npos ? std::string_view(entry.name) : std::string_view(entry.name).substr(slash + 1);
        if (entry.readable && IsReplacementImageName(filename)) images.push_back(entry.name);
    }
    return images;
}

bool HoldsImagesDirectly(const fs::path& folder)
{
    std::error_code error;
    std::size_t seen = 0;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error))
    {
        if (++seen > kMaxFolderEntries) break;
        std::error_code type_error;
        if (it->is_regular_file(type_error) && IsReplacementImageName(PathUtf8(it->path().filename()))) return true;
    }
    return false;
}

std::string Trim(std::string_view value)
{
    const auto start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    return std::string(value.substr(start, value.find_last_not_of(" \t") - start + 1));
}
}

bool IsReplacementImageName(std::string_view filename)
{
    if (filename.compare(0, 5, "tex1_") != 0) return false;
    if (!EndsWithNoCase(filename, ".png") && !EndsWithNoCase(filename, ".dds")) return false;
    const auto stem = filename.substr(0, filename.size() - 4);
    // Extra mip levels (<name>_mip1.png ...) belong to their image.
    const auto mip = stem.rfind("_mip");
    if (mip != std::string_view::npos && mip + 4 < stem.size()
        && std::all_of(stem.begin() + std::ptrdiff_t(mip + 4), stem.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    return true;
}

std::vector<TexturePack> FindTexturePacks(const fs::path& folder)
{
    std::vector<TexturePack> packs;
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) return packs;
    auto folder_pack = [](const fs::path& path) {
        TexturePack pack;
        pack.name = PathUtf8(path.filename());
        pack.path = path;
        pack.textures = FolderImages(path).size();
        return pack;
    };
    // The packs folder is itself a pack (the player chose the pack's folder).
    const auto own_name = PathUtf8(folder.lexically_normal().filename().empty()
                                       ? folder.lexically_normal().parent_path().filename()
                                       : folder.filename());
    if (ThisGameFolder(own_name) || HoldsImagesDirectly(folder))
    {
        auto pack = folder_pack(folder);
        pack.name = own_name;
        if (pack.textures) packs.push_back(std::move(pack));
        return packs;
    }
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error))
    {
        const auto name = PathUtf8(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        std::error_code type_error;
        if (it->is_directory(type_error))
        {
            if (OtherGameFolder(name)) continue;
            auto pack = folder_pack(it->path());
            if (pack.textures) packs.push_back(std::move(pack));
        }
        else if (it->is_regular_file(type_error) && EndsWithNoCase(name, ".zip"))
        {
            TexturePack pack;
            pack.name = name;
            pack.path = it->path();
            pack.archive = true;
            try
            {
                pack.textures = ArchiveImages(ZipArchive(it->path())).size();
                if (!pack.textures) continue; // a zip of something else
            }
            catch (const std::exception& failure)
            {
                pack.error = failure.what();
            }
            packs.push_back(std::move(pack));
        }
    }
    std::sort(packs.begin(), packs.end(), [](const TexturePack& a, const TexturePack& b) { return a.name < b.name; });
    return packs;
}

std::optional<TexturePack> ProbeTexturePack(const fs::path& path)
{
    std::error_code error;
    TexturePack pack;
    pack.name = PathUtf8(path.filename());
    pack.path = path;
    if (fs::is_directory(path, error))
        pack.textures = FolderImages(path).size();
    else if (fs::is_regular_file(path, error) && EndsWithNoCase(pack.name, ".zip"))
    {
        pack.archive = true;
        try
        {
            pack.textures = ArchiveImages(ZipArchive(path)).size();
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }
    if (!pack.textures) return std::nullopt;
    return pack;
}

std::vector<TexturePack> OrderTexturePacks(std::vector<TexturePack> packs, const std::vector<std::string>& order)
{
    std::vector<TexturePack> ordered;
    for (const auto& name : order)
    {
        const auto it = std::find_if(packs.begin(), packs.end(), [&](const TexturePack& pack) { return pack.name == name; });
        if (it == packs.end()) continue;
        ordered.push_back(std::move(*it));
        packs.erase(it);
    }
    for (auto& pack : packs) ordered.push_back(std::move(pack));
    return ordered;
}

std::vector<std::string> SplitPackNames(std::string_view value)
{
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= value.size())
    {
        const auto bar = std::min(value.find('|', start), value.size());
        auto name = Trim(value.substr(start, bar - start));
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(std::move(name));
        start = bar + 1;
    }
    return names;
}

std::string JoinPackNames(const std::vector<std::string>& names)
{
    std::string value;
    for (const auto& name : names)
    {
        if (!StorablePackName(name)) continue;
        if (!value.empty()) value += " | ";
        value += name;
    }
    return value;
}

bool StorablePackName(std::string_view name)
{
    return !name.empty() && Trim(name) == name && name.find_first_of(std::string_view("|\r\n\0", 4)) == std::string_view::npos;
}

TexturePackFiles::TexturePackFiles(const TexturePack& pack, std::string_view game_id) : root_(pack.path)
{
    std::vector<std::string> images;
    if (pack.archive)
    {
        archive_ = std::make_unique<ZipArchive>(pack.path);
        images = ArchiveImages(*archive_);
    }
    else images = FolderImages(pack.path);
    for (auto& image : images)
    {
        bool other_release = false;
        for (std::size_t start = 0, slash; (slash = image.find('/', start)) != std::string::npos; start = slash + 1)
            other_release = other_release || OtherReleaseFolder(std::string_view(image).substr(start, slash - start), game_id);
        if (!other_release) images_.push_back(std::move(image));
    }
    std::sort(images_.begin(), images_.end());
}

bool TexturePackFiles::Read(std::string_view path, std::vector<std::uint8_t>& out) const
{
    out.clear();
    if (archive_)
    {
        const auto* entry = archive_->Find(path);
        return entry && archive_->Read(*entry, out);
    }
    const auto relative = PathFromUtf8(path);
    if (path.empty() || relative.is_absolute() || relative.has_root_path()) return false;
    for (const auto& part : relative)
        if (part == "..") return false;
    const auto file = root_ / relative;
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > kMaxImageFile) return false;
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    try
    {
        out.resize(std::size_t(size));
    }
    catch (const std::bad_alloc&)
    {
        out.clear();
        return false;
    }
    if (size && !input.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)))
    {
        out.clear();
        return false;
    }
    return true;
}
}
