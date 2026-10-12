#include "platform/save_import.h"

#include "platform/filesystem_physical.h"
#include "platform/path.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace mscharged::platform {
namespace {
namespace fs = std::filesystem;
namespace physical = filesystem_detail;

// Same limits as the IOS names filesystem_device.cpp accepts; a Wii save file
// is far below the size limit.
constexpr std::size_t kNameBytes = 12, kMaxFileBytes = 16u << 20, kMaxFiles = 32;

struct Entry {
    std::uint32_t uid{};
    unsigned gid{}, owner{}, group{}, other{}, attr{}, directory{};
    std::uint64_t order{};
};

struct Catalog {
    std::uint64_t next = 1;
    std::map<std::string, Entry> entries;
};

std::string Hex8(std::uint32_t value) {
    std::ostringstream text;
    text << std::hex << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

std::string Home(std::uint64_t title_id) {
    return "/title/" + Hex8(std::uint32_t(title_id >> 32)) + '/' + Hex8(std::uint32_t(title_id)) + "/data";
}

std::string Parent(const std::string& path) {
    const auto end = path.rfind('/');
    return end == 0 ? "/" : path.substr(0, end);
}

// Physical location of an IOS path, encoded the way the device stores it.
fs::path Physical(const fs::path& data, const std::string& path) {
    auto value = data;
    std::size_t begin = 1;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        value /= physical::Component(path.substr(begin, (end == std::string::npos ? path.size() : end) - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return value;
}

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > kNameBytes || name == "." || name == "..") return false;
    for (const unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\') return false;
    return true;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + PathUtf8(path));
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (!input && !input.eof()) throw std::runtime_error("Cannot read " + PathUtf8(path));
    return bytes.str();
}

// Write through a staging file next to the target, then replace it.
void WriteAtomically(const fs::path& target, const std::string& bytes) {
    auto staging = target;
    staging += ".import";
    const auto file = physical::OpenStaging(staging);
    if (!physical::Valid(file)) throw std::system_error(errno, std::generic_category(), PathUtf8(staging));
    std::size_t written = 0;
    int failure = 0;
    while (written < bytes.size()) {
        const auto result = physical::Write(file, bytes.data() + written, bytes.size() - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) { failure = result < 0 ? errno : EIO; break; }
        written += static_cast<std::size_t>(result);
    }
    if (!failure && physical::Sync(file)) failure = errno;
    if (physical::Close(file) && !failure) failure = errno;
    if (!failure && physical::Replace(staging, target)) failure = errno;
    if (failure) {
        physical::Remove(staging, false);
        throw std::system_error(failure, std::generic_category(), PathUtf8(target));
    }
}

Catalog LoadCatalog(const fs::path& metadata, const SaveTitle& title) {
    std::istringstream input(ReadFile(metadata));
    std::string signature;
    unsigned version{}, gid{};
    std::uint64_t title_id{};
    std::uint32_t uid{};
    Catalog catalog;
    input >> signature >> version >> title_id >> uid >> gid >> catalog.next;
    if (!input || signature != physical::CatalogSignature || version != 1 || !catalog.next)
        throw std::runtime_error("The save folder's catalog (metadata.txt) is not readable.");
    if (title_id != title.title_id || uid != title.uid || gid != title.gid)
        throw std::runtime_error("The save folder belongs to a different game.");
    std::string path;
    while (input >> std::quoted(path)) {
        Entry entry;
        input >> entry.uid >> entry.gid >> entry.owner >> entry.group >> entry.other >> entry.attr >> entry.directory
            >> entry.order;
        if (!input || entry.directory > 1 || !entry.order || entry.order >= catalog.next
            || !catalog.entries.emplace(path, entry).second)
            throw std::runtime_error("The save folder's catalog (metadata.txt) is damaged.");
    }
    if (!input.eof()) throw std::runtime_error("The save folder's catalog (metadata.txt) is damaged.");
    return catalog;
}

// The layout InitializeNativeFilesystem creates for a new backing.
Catalog NewCatalog(const fs::path& data, const SaveTitle& title) {
    Catalog catalog;
    const auto home = Home(title.title_id);
    for (const std::string& path : {std::string("/"), std::string("/tmp"), std::string("/title"),
                                    Parent(Parent(home)), Parent(home), home}) {
        physical::MakeDirs(Physical(data, path));
        const bool owned = path == home;
        catalog.entries.emplace(path, Entry{owned ? title.uid : 0, owned ? title.gid : 0u, 3, owned ? 0u : 3u,
                                            owned ? 0u : path == "/tmp" ? 3u : 1u, 0, 1, catalog.next++});
    }
    return catalog;
}

std::string CatalogText(const Catalog& catalog, const SaveTitle& title) {
    std::ostringstream text;
    text << physical::CatalogSignature << " 1\n"
         << title.title_id << ' ' << title.uid << ' ' << title.gid << ' ' << catalog.next << '\n';
    for (const auto& [path, m] : catalog.entries)
        text << std::quoted(path) << ' ' << m.uid << ' ' << m.gid << ' ' << m.owner << ' ' << m.group << ' '
             << m.other << ' ' << m.attr << ' ' << m.directory << ' ' << m.order << '\n';
    return text.str();
}

std::string Timestamp() {
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local);
    return text;
}

fs::path Environment(const char* name) {
#ifdef _WIN32
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    const wchar_t* value = _wgetenv(wide.c_str());
    return value && *value ? fs::path(value) : fs::path();
#else
    const char* value = std::getenv(name);
    return value && *value ? PathFromUtf8(value) : fs::path();
#endif
}

bool IsDirectory(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

bool IsFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}
} // namespace

fs::path DolphinSaveFolder(const fs::path& dolphin_user, std::uint64_t title_id) {
    return dolphin_user / "Wii" / "title" / Hex8(std::uint32_t(title_id >> 32)) / Hex8(std::uint32_t(title_id))
        / "data";
}

std::vector<fs::path> FindDolphinSaves(std::uint64_t title_id) {
    std::vector<fs::path> users;
    const auto home = Environment("HOME");
#if defined(_WIN32)
    const auto profile = Environment("USERPROFILE");
    if (!profile.empty()) users.push_back(profile / "Documents" / "Dolphin Emulator");
    const auto onedrive = Environment("OneDrive");
    if (!onedrive.empty()) users.push_back(onedrive / "Documents" / "Dolphin Emulator");
    const auto appdata = Environment("APPDATA");
    if (!appdata.empty()) users.push_back(appdata / "Dolphin Emulator");
#elif defined(__APPLE__)
    if (!home.empty()) users.push_back(home / "Library" / "Application Support" / "Dolphin");
#else
    const auto data_home = Environment("XDG_DATA_HOME");
    if (!data_home.empty()) users.push_back(data_home / "dolphin-emu");
    if (!home.empty()) {
        users.push_back(home / ".local" / "share" / "dolphin-emu");
        users.push_back(home / ".var" / "app" / "org.DolphinEmu.dolphin-emu" / "data" / "dolphin-emu");
        users.push_back(home / ".dolphin-emu");
    }
#endif
    std::vector<fs::path> saves;
    for (const auto& user : users) {
        const auto folder = DolphinSaveFolder(user, title_id);
        if (!IsDirectory(folder)) continue;
        bool known = false;
        for (const auto& save : saves) {
            std::error_code error;
            known = known || fs::equivalent(save, folder, error);
        }
        if (!known) saves.push_back(folder);
    }
    return saves;
}

fs::path ResolveDolphinSave(const fs::path& chosen, std::uint64_t title_id, const std::string& required) {
    const auto start = IsFile(chosen) ? chosen.parent_path() : chosen;
    const auto low = Hex8(std::uint32_t(title_id)), high = Hex8(std::uint32_t(title_id >> 32));
    for (const auto& folder : {start, start / "data", start / low / "data", start / high / low / "data",
                               start / "title" / high / low / "data", DolphinSaveFolder(start, title_id)})
        if (IsFile(folder / required)) return folder;
    return {};
}

SaveImportResult ImportDolphinSave(const fs::path& source, const fs::path& nand_root, const SaveTitle& title,
                                   const std::string& required, const fs::path& backup_parent) {
    // Read the whole source first: nothing changes unless all of it is usable.
    std::map<std::string, std::string> files;
    std::error_code error;
    for (fs::directory_iterator it(source, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const auto name = PathUtf8(it->path().filename());
        if (!ValidName(name)) throw std::runtime_error("The save folder has a file the Wii cannot store: " + name);
        if (it->file_size(error) > kMaxFileBytes) throw std::runtime_error("The save file " + name + " is too large.");
        if (files.size() == kMaxFiles) throw std::runtime_error("The save folder has too many files.");
        files.emplace(name, ReadFile(it->path()));
    }
    if (error) throw std::runtime_error("Cannot read the save folder " + PathUtf8(source) + ": " + error.message());
    if (!files.count(required))
        throw std::runtime_error("No " + required + " save file in " + PathUtf8(source) + ".");

    const auto root = physical::PrepareRoot(nand_root);
    const auto data = root / "data", metadata = root / "metadata.txt";
    const bool existing = physical::Exists(metadata);
    if (!existing && physical::Exists(data) && !physical::IsEmpty(data))
        throw std::runtime_error("The save folder " + PathUtf8(root) + " has files but no catalog (metadata.txt).");
    auto catalog = existing ? LoadCatalog(metadata, title) : Catalog{};
    const auto home = Home(title.title_id);
    if (existing) {
        const auto entry = catalog.entries.find(home);
        if (entry == catalog.entries.end() || !entry->second.directory)
            throw std::runtime_error("The save folder's catalog (metadata.txt) has no save folder for this game.");
        for (const auto& [name, bytes] : files) {
            const auto file = catalog.entries.find(home + '/' + name);
            if (file != catalog.entries.end() && file->second.directory)
                throw std::runtime_error("The save folder has a folder named " + name + ".");
        }
    }

    // Back up what is replaced: the catalog and existing files of the same names.
    SaveImportResult result;
    if (existing) {
        result.backup = backup_parent / ("before-dolphin-import-" + Timestamp());
        fs::create_directories(result.backup);
        WriteAtomically(result.backup / "metadata.txt", ReadFile(metadata));
        for (const auto& [name, bytes] : files)
            if (catalog.entries.count(home + '/' + name))
                WriteAtomically(result.backup / PathFromUtf8(name), ReadFile(Physical(data, home + '/' + name)));
    } else {
        physical::MakeDirs(data);
        catalog = NewCatalog(data, title);
    }

    for (const auto& [name, bytes] : files) {
        const auto path = home + '/' + name;
        WriteAtomically(Physical(data, path), bytes);
        // New files get the permissions the game gives its own (owner read/write).
        if (!catalog.entries.count(path))
            catalog.entries.emplace(path, Entry{title.uid, title.gid, 3, 0, 0, 0, 0, catalog.next++});
        result.files.push_back(name);
    }
    WriteAtomically(metadata, CatalogText(catalog, title));
    return result;
}
} // namespace mscharged::platform
