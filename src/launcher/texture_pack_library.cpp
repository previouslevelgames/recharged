#include "launcher/texture_pack_library.h"

#include "bootstrap/texts.h"
#include "platform/path.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace mscharged::launcher
{
namespace fs = std::filesystem;

TexturePackLibrary::~TexturePackLibrary() { stop_->store(true); }

bool TexturePackLibrary::Add(const fs::path& source, const fs::path& folder)
{
    // Packs are folders or .zip files (platform::ProbeTexturePack): anything
    // else is no pack, without reading it.
    std::string extension = PathUtf8(source.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    std::error_code error;
    if (extension != ".zip" && !fs::is_directory(source, error)) return false;
    if (Checking() || Copying())
    {
        pending_.push_back({Event::Kind::CopyBusy, adding_name_, {}, source});
        return true;
    }
    adding_source_ = source;
    adding_folder_ = folder;
    adding_name_ = PathUtf8(source.filename());
    adding_textures_ = 0;
    // Reading a large folder takes a while: not on the window's thread.
    probe_.Start([source] { return platform::ProbeTexturePack(source); });
    return true;
}

std::vector<TexturePackLibrary::Event> TexturePackLibrary::Poll(const fs::path& folder, bool shown)
{
    std::vector<Event> events = std::move(pending_);
    pending_.clear();
    if (auto scan = scan_.Take())
    {
        packs_ = std::move(scan->packs);
        folder_exists_ = scan->folder_exists;
    }
    if (auto probed = probe_.Take())
    {
        std::error_code error;
        if (!*probed) events.push_back({Event::Kind::NotAPack, {}, {}, adding_source_});
        else if (fs::equivalent(adding_source_.parent_path(), adding_folder_, error))
            events.push_back({Event::Kind::AlreadyInFolder, (*probed)->name, {}, adding_source_});
        else if (fs::exists(adding_folder_ / adding_source_.filename(), error))
            events.push_back({Event::Kind::FolderHasPack, (*probed)->name, {}, adding_source_});
        else
        {
            adding_name_ = (*probed)->name;
            adding_textures_ = (*probed)->textures;
            copy_.Start([source = adding_source_, target = adding_folder_, stop = stop_] {
                return CopyTexturePack(source, target, *stop);
            });
        }
    }
    if (auto copied = copy_.Take())
    {
        events.push_back({copied->empty() ? Event::Kind::Added : Event::Kind::AddFailed, adding_name_, *copied,
                          adding_source_});
        rescan_ = true;
    }
    const bool opened = shown && !shown_;
    shown_ = shown;
    if (shown && (opened || PathUtf8(folder) != scanned_folder_)) rescan_ = true;
    if (shown && rescan_ && !scan_.Busy())
    {
        rescan_ = false;
        scanned_folder_ = PathUtf8(folder);
        scan_.Start([folder] {
            std::error_code error;
            return Scan{platform::FindTexturePacks(folder), fs::is_directory(folder, error)};
        });
    }
    return events;
}

std::optional<std::string> PackOrderAfterMove(std::vector<platform::TexturePack> ordered, std::size_t from,
                                              std::size_t to)
{
    if (from >= ordered.size() || to >= ordered.size() || from == to) return std::nullopt;
    auto pack = std::move(ordered[from]);
    ordered.erase(ordered.begin() + std::ptrdiff_t(from));
    ordered.insert(ordered.begin() + std::ptrdiff_t(to), std::move(pack));
    std::vector<std::string> names;
    for (const auto& item : ordered) names.push_back(item.name);
    return platform::JoinPackNames(names);
}

std::string PacksOffWith(const std::string& off, const std::string& name, bool on)
{
    auto names = platform::SplitPackNames(off);
    names.erase(std::remove(names.begin(), names.end(), name), names.end());
    if (!on) names.push_back(name);
    return platform::JoinPackNames(names);
}

std::string CopyTexturePack(const fs::path& source, const fs::path& folder, const std::atomic<bool>& stop)
{
    std::error_code error;
    fs::create_directories(folder, error);
    if (error)
        return texts::T("settings.gameplay.mods.cannot_create", {{"path", PathUtf8(folder)}, {"error", error.message()}});
    const auto name = source.filename();
    const auto temporary = folder / PathFromUtf8("." + PathUtf8(name) + ".adding");
    fs::remove_all(temporary, error);
    error.clear();
    if (fs::is_directory(source, error))
    {
        // File by file, so that closing the launcher stops a large copy.
        fs::create_directories(temporary, error);
        for (fs::recursive_directory_iterator it(source, error), end; !error && it != end; it.increment(error))
        {
            if (stop.load())
            {
                error = std::make_error_code(std::errc::operation_canceled);
                break;
            }
            const fs::path target = temporary / it->path().lexically_relative(source);
            if (it->is_directory(error)) fs::create_directories(target, error);
            else if (!error) fs::copy_file(it->path(), target, error);
        }
    }
    else if (!error)
        fs::copy_file(source, temporary, error);
    if (!error) fs::rename(temporary, folder / name, error);
    if (!error) return {};
    std::error_code ignored;
    fs::remove_all(temporary, ignored);
    return texts::T("settings.gameplay.mods.cannot_add", {{"name", PathUtf8(name)}, {"error", error.message()}});
}
} // namespace mscharged::launcher
