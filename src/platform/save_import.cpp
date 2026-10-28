#include "platform/save_import.h"

#include "platform/filesystem_physical.h"
#include "platform/path.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
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

// The Mii database (RFLiDatabase, RFL_Database.c): "RNOD", 100 Miis of 0x4A
// bytes, then the hidden Miis. The Mii library loads the first 0x1F1E0 bytes
// and rejects them unless their CRC comes out 0.
const std::string kFaceLib = "/shared2/menu/FaceLib", kMiiDatabase = kFaceLib + "/RFL_DB.dat";
constexpr std::size_t kMiiDatabaseBytes = 0xBE6C0, kMiiCheckedBytes = 0x1F1E0, kMiiSlots = 100, kMiiBytes = 0x4A;
constexpr std::size_t kMiiNameOffset = 0x2, kMiiNameChars = 10, kMiiCreateIdOffset = 0x18, kMiiCreateIdBytes = 8;
// A Mii's first 16 bits (big-endian) end with its favourite colour (4 bits)
// and whether it is a favourite (RFLiCharData).
constexpr unsigned kMiiColorShift = 1, kMiiColorMask = 0xF, kMiiFavoriteBit = 1;

// The game save (SaveLoad.cpp), as the Wii stores it (big-endian): the size
// of its data and their checksum, then the data, padded to 32 bytes. The
// data start with the user's settings (UserInfo, 0x80 bytes) and the captains'
// lineups (mRulesTable: 12 x 3 sidekicks, 32 bits each).
const std::string kGameSave = "Strikers2";
constexpr std::size_t kSaveHeaderBytes = 8, kSaveAlign = 32, kLineupsOffset = kSaveHeaderBytes + 0x80;
// UserInfo's GameplaySettings and CheatSettings (3 x 32 bits) by MatchMode:
// mGameplayOptions and mCheatOptions, then their Alt twins. The settings
// screen's part of GameplaySettings is its first five 32-bit values; the
// bytes after them (power-ups, Mega Strikes and skillshots by team, and one
// more) are left as they are.
constexpr std::size_t kGameplayOffsets[kMatchModeCount] = {kSaveHeaderBytes + 0x24, kSaveHeaderBytes + 0x4C};
constexpr std::size_t kCheatOffsets[kMatchModeCount] = {kSaveHeaderBytes + 0x40, kSaveHeaderBytes + 0x68};
constexpr std::size_t kLineupsBytes = kCaptainCount * kSidekickSlots * 4;
constexpr int kDefaultLineups[kCaptainCount][kSidekickSlots] = {
    {1, 1, 7}, {4, 5, 5}, {6, 1, 5}, {0, 0, 0}, {7, 7, 1}, {3, 3, 0},
    {2, 5, 2}, {3, 4, 5}, {2, 1, 5}, {5, 1, 2}, {6, 2, 6}, {6, 0, 0},
};

// The online save (SaveLoad.cpp), in the data folder's nocopy folder, which
// the Wii never copies: the same size and checksum, then ten slots of 0xB10
// bytes (GameInfoSaveSlot), one for each Mii that went online. A slot starts
// with the Mii's DWC user data (DWCAccUserData: its size, 64, the profile ID
// at 0x1C, the game's code at 0x24, a CRC-32 of the rest at 0x3C) and holds
// the Mii's create ID at 0xA80.
const std::string kOnlineSave = "nocopy/Online";
constexpr std::size_t kOnlineSlots = 10, kOnlineSlotBytes = 0xB10;
constexpr std::size_t kOnlineSaveBytes = kSaveHeaderBytes + kOnlineSlots * kOnlineSlotBytes;
constexpr std::size_t kUserDataBytes = 0x40, kProfileIdOffset = 0x1C, kGameCodeOffset = 0x24, kUserDataCrcOffset = 0x3C;
constexpr std::size_t kSlotCreateIdOffset = 0xA80;

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

// RFLiCalculateCRC (RFL_Database.c).
std::uint16_t MiiCrc(const std::string& bytes, std::size_t size) {
    std::uint16_t crc = 0;
    for (std::size_t n = 0; n < size; ++n) {
        auto data = static_cast<unsigned char>(bytes[n]);
        for (int bit = 0; bit < 8; ++bit, data <<= 1) {
            crc = std::uint16_t(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
            if (data & 0x80) crc ^= 1;
        }
    }
    return crc;
}

bool ValidMiiDatabase(const std::string& bytes) {
    return bytes.size() == kMiiDatabaseBytes && bytes.compare(0, 4, "RNOD") == 0
        && MiiCrc(bytes, kMiiCheckedBytes) == 0;
}

// A Mii's name: up to 10 UTF-16 (big-endian) characters, 0 after the last.
std::string MiiName(const unsigned char* name) {
    std::string text;
    for (std::size_t n = 0; n < kMiiNameChars; ++n) {
        std::uint32_t c = (name[2 * n] << 8) | name[2 * n + 1];
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && n + 1 < kMiiNameChars) {
            const std::uint32_t low = (name[2 * n + 2] << 8) | name[2 * n + 3];
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++n;
            }
        }
        if (c < 0x80) text += char(c);
        else if (c < 0x800) text += {char(0xC0 | c >> 6), char(0x80 | (c & 0x3F))};
        else if (c < 0x10000) text += {char(0xE0 | c >> 12), char(0x80 | (c >> 6 & 0x3F)), char(0x80 | (c & 0x3F))};
        else text += {char(0xF0 | c >> 18), char(0x80 | (c >> 12 & 0x3F)), char(0x80 | (c >> 6 & 0x3F)),
                      char(0x80 | (c & 0x3F))};
    }
    return text;
}

// A Mii record of the database, and whether its slot holds a Mii: its create
// ID is not all zero (RFLiIsValidID).
const unsigned char* MiiRecord(const std::string& database, std::size_t slot) {
    return reinterpret_cast<const unsigned char*>(database.data()) + 4 + slot * kMiiBytes;
}

bool MiiUsed(const unsigned char* mii) {
    for (std::size_t n = 0; n < kMiiCreateIdBytes; ++n)
        if (mii[kMiiCreateIdOffset + n]) return true;
    return false;
}

// The Miis of a database, empty slots left out.
std::vector<std::string> MiiNames(const std::string& database) {
    std::vector<std::string> names;
    for (std::size_t slot = 0; slot < kMiiSlots; ++slot)
        if (MiiUsed(MiiRecord(database, slot))) names.push_back(MiiName(MiiRecord(database, slot) + kMiiNameOffset));
    return names;
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

// The catalog of the backing at `root`, or an empty one (`existing` false)
// when the backing is new. A backing with files but no catalog is not used.
Catalog ExistingCatalog(const fs::path& root, const SaveTitle& title, bool& existing) {
    const auto data = root / "data", metadata = root / "metadata.txt";
    existing = physical::Exists(metadata);
    if (!existing && physical::Exists(data) && !physical::IsEmpty(data))
        throw std::runtime_error("The save folder " + PathUtf8(root) + " has files but no catalog (metadata.txt).");
    return existing ? LoadCatalog(metadata, title) : Catalog{};
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

// A new folder for one backup. Another backup in the same second gets its own
// (<name>-2, <name>-3...) instead of overwriting the first.
fs::path NewBackupFolder(const fs::path& parent, const std::string& prefix) {
    const auto name = prefix + Timestamp();
    auto folder = parent / name;
    std::error_code error;
    for (int n = 2; fs::exists(folder, error) || error; ++n) {
        if (error) throw std::system_error(error, PathUtf8(folder));
        folder = parent / (name + '-' + std::to_string(n));
    }
    fs::create_directories(folder);
    return folder;
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

// Dolphin's usual user directories on this system.
std::vector<fs::path> DolphinUsers() {
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
    return users;
}

// Add `path` unless `paths` has it already (the same folder can be reached
// through two of Dolphin's user directories).
void AddUnique(std::vector<fs::path>& paths, const fs::path& path) {
    for (const auto& known : paths) {
        std::error_code error;
        if (fs::equivalent(known, path, error)) return;
    }
    paths.push_back(path);
}
} // namespace

fs::path DolphinSaveFolder(const fs::path& dolphin_user, std::uint64_t title_id) {
    return dolphin_user / "Wii" / "title" / Hex8(std::uint32_t(title_id >> 32)) / Hex8(std::uint32_t(title_id))
        / "data";
}

std::vector<fs::path> FindDolphinSaves(std::uint64_t title_id) {
    std::vector<fs::path> saves;
    for (const auto& user : DolphinUsers()) {
        const auto folder = DolphinSaveFolder(user, title_id);
        if (IsDirectory(folder)) AddUnique(saves, folder);
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
    bool existing{};
    auto catalog = ExistingCatalog(root, title, existing);
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
        result.backup = NewBackupFolder(backup_parent, "before-dolphin-import-");
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

fs::path DolphinMiiDatabase(const fs::path& dolphin_user) {
    return dolphin_user / "Wii" / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat";
}

std::vector<fs::path> FindDolphinMiiDatabases() {
    std::vector<fs::path> databases;
    for (const auto& user : DolphinUsers()) {
        const auto database = DolphinMiiDatabase(user);
        if (IsFile(database)) AddUnique(databases, database);
    }
    return databases;
}

fs::path ResolveDolphinMiiDatabase(const fs::path& chosen) {
    const auto start = IsFile(chosen) ? chosen.parent_path() : chosen;
    for (const auto& folder : {start, start / "FaceLib", start / "menu" / "FaceLib",
                               start / "shared2" / "menu" / "FaceLib", DolphinMiiDatabase(start).parent_path()})
        if (IsFile(folder / "RFL_DB.dat")) return folder / "RFL_DB.dat";
    return {};
}

namespace {
// The backing a Mii database goes to: its catalog, checked for the database's
// folders and file, and the database it holds (empty when it has none).
struct MiiBacking {
    fs::path data, metadata;
    bool existing{};
    Catalog catalog;
    std::string database;
};

const std::vector<std::string>& MiiFolders() {
    static const std::vector<std::string> folders{"/shared2", "/shared2/menu", kFaceLib};
    return folders;
}

// The database's folders and file must be what the catalog says they are.
void CheckMiiEntries(const Catalog& catalog) {
    for (const auto& path : MiiFolders()) {
        const auto entry = catalog.entries.find(path);
        if (entry != catalog.entries.end() && !entry->second.directory)
            throw std::runtime_error("The save folder's catalog (metadata.txt) has a file named " + path + ".");
    }
    const auto database = catalog.entries.find(kMiiDatabase);
    if (database != catalog.entries.end() && database->second.directory)
        throw std::runtime_error("The save folder's catalog (metadata.txt) has a folder named " + kMiiDatabase + ".");
}

MiiBacking OpenMiiBacking(const fs::path& nand_root, const SaveTitle& title) {
    const auto root = physical::PrepareRoot(nand_root);
    MiiBacking backing{root / "data", root / "metadata.txt"};
    backing.catalog = ExistingCatalog(root, title, backing.existing);
    CheckMiiEntries(backing.catalog);
    // A database the catalog has but the disk lost holds nothing to keep.
    const auto file = Physical(backing.data, kMiiDatabase);
    if (backing.catalog.entries.count(kMiiDatabase) && IsFile(file)) backing.database = ReadFile(file);
    return backing;
}

// Write `database` as the backing's Mii database. The catalog and the
// database it replaces are first copied to `backup_parent`/<prefix><time>,
// which is returned (empty for a new backing).
fs::path WriteMiiDatabase(MiiBacking& backing, const SaveTitle& title, const std::string& database,
                          const fs::path& backup_parent, const std::string& prefix) {
    fs::path backup;
    auto& catalog = backing.catalog;
    const bool replacing = catalog.entries.count(kMiiDatabase) != 0;
    if (backing.existing) {
        backup = NewBackupFolder(backup_parent, prefix);
        WriteAtomically(backup / "metadata.txt", ReadFile(backing.metadata));
        if (!backing.database.empty()) WriteAtomically(backup / "RFL_DB.dat", backing.database);
    } else {
        physical::MakeDirs(backing.data);
        catalog = NewCatalog(backing.data, title);
    }

    // Every game reads and writes the database (the Mii library keeps the
    // hidden Miis in it): the shared folders and the file belong to no title
    // and are read/write for all. Entries the backing has already stay.
    for (const auto& path : MiiFolders()) {
        if (catalog.entries.count(path)) continue;
        physical::MakeDirs(Physical(backing.data, path));
        catalog.entries.emplace(path, Entry{0, 0, 3, 3, 3, 0, 1, catalog.next++});
    }
    WriteAtomically(Physical(backing.data, kMiiDatabase), database);
    if (!replacing) catalog.entries.emplace(kMiiDatabase, Entry{0, 0, 3, 3, 3, 0, 0, catalog.next++});
    WriteAtomically(backing.metadata, CatalogText(catalog, title));
    return backup;
}

// Store the CRC that makes the checked bytes' CRC come out 0: the CRC of the
// rest with its field 0.
void SetMiiCrc(std::string& database) {
    database[kMiiCheckedBytes - 2] = database[kMiiCheckedBytes - 1] = 0;
    const auto crc = MiiCrc(database, kMiiCheckedBytes);
    database[kMiiCheckedBytes - 2] = char(crc >> 8);
    database[kMiiCheckedBytes - 1] = char(crc);
}

// The database the Mii library formats (RFL_Format.c): no Miis, isolation
// set, an empty list of hidden Miis (head, tail and every link -1), zeros
// for the hidden Miis themselves, and its CRC.
std::string EmptyMiiDatabase() {
    std::string database(kMiiDatabaseBytes, '\0');
    database.replace(0, 4, "RNOD");
    database[0x1CEC] = char(0x80);
    database.replace(0x1D00, 8, "RNHD\xFF\xFF\xFF\xFF");
    for (std::size_t entry = 0; entry < 10000; ++entry)
        database.replace(0x1D08 + entry * 12 + 8, 4, "\x7F\xFF\x7F\xFF");
    SetMiiCrc(database);
    return database;
}

std::uint32_t Big32(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t n = 0; n < 4; ++n) value = value << 8 | static_cast<unsigned char>(bytes[offset + n]);
    return value;
}

void PutBig32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t n = 0; n < 4; ++n) bytes[offset + n] = char(value >> (24 - 8 * n));
}

std::uint32_t Little32(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t n = 4; n-- > 0;) value = value << 8 | static_cast<unsigned char>(bytes[offset + n]);
    return value;
}

// The usual CRC-32 (nlChecksum32 in nlMain.cpp, and DWC's MATH_CalcCRC32).
std::uint32_t Crc32(const std::string& bytes, std::size_t begin, std::size_t end) {
    std::uint32_t crc = ~0u;
    for (std::size_t n = begin; n < end; ++n) {
        crc ^= static_cast<unsigned char>(bytes[n]);
        for (int bit = 0; bit < 8; ++bit) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

// nlChecksum32: the save's CRC-32, over everything after the header.
std::uint32_t SaveCrc(const std::string& save) {
    return Crc32(save, kSaveHeaderBytes, save.size());
}

bool ValidLineups(const SidekickLineups& lineups) {
    for (const auto& lineup : lineups)
        for (const int sidekick : lineup)
            if (sidekick < 0 || sidekick >= int(kSidekickCount)) return false;
    return true;
}

// A game save the game accepts: its file is its data's size rounded up to
// 32 bytes (what SaveLoad reads), it holds the settings and lineups, and the
// checksum matches.
bool AcceptedSave(const std::string& save) {
    if (save.size() < kLineupsOffset + kLineupsBytes || save.size() % kSaveAlign) return false;
    const auto size = Big32(save, 0);
    return size >= kLineupsOffset + kLineupsBytes && (size + kSaveAlign - 1) / kSaveAlign * kSaveAlign == save.size()
        && Big32(save, 4) == SaveCrc(save);
}

// The lineups of a game save the game accepts, when every sidekick is one of
// the eight. Empty otherwise.
std::optional<SidekickLineups> SaveLineups(const std::string& save) {
    if (!AcceptedSave(save)) return std::nullopt;
    SidekickLineups lineups{};
    for (std::size_t captain = 0; captain < kCaptainCount; ++captain)
        for (std::size_t slot = 0; slot < kSidekickSlots; ++slot)
            lineups[captain][slot] = int(Big32(save, kLineupsOffset + (captain * kSidekickSlots + slot) * 4));
    if (!ValidLineups(lineups)) return std::nullopt;
    return lineups;
}

MatchSettings SaveMatch(const std::string& save, MatchMode mode) {
    const auto gameplay = kGameplayOffsets[std::size_t(mode)], cheats = kCheatOffsets[std::size_t(mode)];
    MatchSettings settings;
    settings.skill = int(Big32(save, gameplay));
    settings.goal_limit = Big32(save, gameplay + 0x4) == 1; // eGameLimitType: 0 time, 1 goals
    settings.time = int(Big32(save, gameplay + 0x8));
    settings.goals = int(Big32(save, gameplay + 0xC));
    settings.best_of = int(Big32(save, gameplay + 0x10));
    settings.powerup_cheat = int(Big32(save, cheats));
    settings.stadium_cheat = int(Big32(save, cheats + 0x4));
    settings.player_cheat = int(Big32(save, cheats + 0x8));
    return settings;
}

void PutMatch(std::string& save, MatchMode mode, const MatchSettings& settings) {
    const auto gameplay = kGameplayOffsets[std::size_t(mode)], cheats = kCheatOffsets[std::size_t(mode)];
    PutBig32(save, gameplay, std::uint32_t(settings.skill));
    PutBig32(save, gameplay + 0x4, settings.goal_limit ? 1u : 0u);
    PutBig32(save, gameplay + 0x8, std::uint32_t(settings.time));
    PutBig32(save, gameplay + 0xC, std::uint32_t(settings.goals));
    PutBig32(save, gameplay + 0x10, std::uint32_t(settings.best_of));
    PutBig32(save, cheats, std::uint32_t(settings.powerup_cheat));
    PutBig32(save, cheats + 0x4, std::uint32_t(settings.stadium_cheat));
    PutBig32(save, cheats + 0x8, std::uint32_t(settings.player_cheat));
}

// The game save's file, or empty when the backing has none. Throws for a
// catalog that cannot be used, or a folder in the save's place.
fs::path GameSaveFile(const fs::path& nand_root, const SaveTitle& title) {
    bool existing{};
    const auto catalog = ExistingCatalog(nand_root, title, existing);
    const auto path = Home(title.title_id) + '/' + kGameSave;
    const auto entry = catalog.entries.find(path);
    if (entry == catalog.entries.end()) return {};
    if (entry->second.directory)
        throw std::runtime_error("The save folder's catalog (metadata.txt) has a folder named " + kGameSave + ".");
    const auto file = Physical(nand_root / "data", path);
    return IsFile(file) ? file : fs::path{};
}

// The game save to change, read whole. Throws when there is none, or the
// game would reject it.
std::string SaveToChange(const fs::path& file) {
    if (file.empty()) throw std::runtime_error("There is no game save yet: play once, and the game makes it.");
    auto save = ReadFile(file);
    if (!AcceptedSave(save)) throw std::runtime_error("The game cannot read its save, so it is left as it is.");
    return save;
}

// Store a changed game save with its new checksum. With a `backup_parent`,
// the save it replaces is first copied to `backup_parent`/<prefix><time>,
// which is returned. The catalog stays as it is: the file keeps its name,
// size and permissions.
fs::path StoreGameSave(const fs::path& file, std::string save, const fs::path& backup_parent,
                       const std::string& prefix) {
    PutBig32(save, 4, SaveCrc(save));
    fs::path backup;
    if (!backup_parent.empty()) {
        backup = NewBackupFolder(backup_parent, prefix);
        WriteAtomically(backup / PathFromUtf8(kGameSave), ReadFile(file));
    }
    WriteAtomically(file, save);
    return backup;
}

// A slot of the online save: the Mii it belongs to and its friend code.
struct OnlineSlot {
    std::array<unsigned char, kMiiCreateIdBytes> create_id{};
    std::uint64_t friend_code = 0;
};

// The friend code of the slot at `slot` in the online save, 0 when there is
// none: before the online server gave the Mii its profile, or when the game
// would make new user data at its next login (DWC_CheckUserData fails). The
// port writes the slots in the PC's byte order (save_data.cpp), the Wii in
// its own: the size word tells which.
std::uint64_t SlotFriendCode(const std::string& save, std::size_t slot) {
    const bool little = Little32(save, slot) == kUserDataBytes;
    if (!little && Big32(save, slot) != kUserDataBytes) return 0;
    // DWC's CRC covers the user data as little-endian words.
    std::string data(kUserDataBytes, '\0');
    for (std::size_t n = 0; n < kUserDataBytes; n += 4) {
        const auto word = little ? Little32(save, slot + n) : Big32(save, slot + n);
        for (std::size_t k = 0; k < 4; ++k) data[n + k] = char(word >> (8 * k));
    }
    const auto profile = std::int32_t(Little32(data, kProfileIdOffset));
    if (Crc32(data, 0, kUserDataCrcOffset) != Little32(data, kUserDataCrcOffset) || profile <= 0) return 0;
    return FriendCode(profile, Little32(data, kGameCodeOffset));
}

// The slots of the backing's online save that hold a Mii; none when it has
// no online save the game accepts (the game deletes a damaged one) or it
// cannot be read: the Miis are listed without codes then.
std::vector<OnlineSlot> OnlineSlots(const fs::path& nand_root, const Catalog& catalog, const SaveTitle& title) {
    const auto path = Home(title.title_id) + '/' + kOnlineSave;
    const auto entry = catalog.entries.find(path);
    const auto file = Physical(nand_root / "data", path);
    if (entry == catalog.entries.end() || entry->second.directory || !IsFile(file)) return {};
    std::string save;
    try { save = ReadFile(file); } catch (const std::exception&) { return {}; }
    if (save.size() != (kOnlineSaveBytes + kSaveAlign - 1) / kSaveAlign * kSaveAlign
        || Big32(save, 0) != kOnlineSaveBytes || Big32(save, 4) != SaveCrc(save))
        return {};
    std::vector<OnlineSlot> slots;
    for (std::size_t n = 0; n < kOnlineSlots; ++n) {
        const auto slot = kSaveHeaderBytes + n * kOnlineSlotBytes;
        OnlineSlot online;
        std::copy_n(save.begin() + std::ptrdiff_t(slot + kSlotCreateIdOffset), kMiiCreateIdBytes,
                    online.create_id.begin());
        if (std::all_of(online.create_id.begin(), online.create_id.end(), [](unsigned char c) { return !c; }))
            continue;
        online.friend_code = SlotFriendCode(save, slot);
        slots.push_back(online);
    }
    return slots;
}

// The Miis of a database in the order of the game's online list
// (SHOnlineMiiSelect::BuildMiiList), with their friend codes.
std::vector<InstalledMii> ListedMiis(const std::string& database, const std::vector<OnlineSlot>& slots) {
    std::vector<std::pair<int, InstalledMii>> listed;
    for (std::size_t n = 0; n < kMiiSlots; ++n) {
        const auto* record = MiiRecord(database, n);
        if (!MiiUsed(record)) continue;
        const unsigned flags = record[0] << 8 | record[1];
        InstalledMii mii{MiiName(record + kMiiNameOffset), int(flags >> kMiiColorShift & kMiiColorMask)};
        const auto online = std::find_if(slots.begin(), slots.end(), [&](const OnlineSlot& slot) {
            return std::equal(slot.create_id.begin(), slot.create_id.end(), record + kMiiCreateIdOffset);
        });
        if (online != slots.end()) mii.friend_code = online->friend_code;
        // The list's four passes: Miis with a slot, then without, favourites first in both.
        const int pass = (online == slots.end() ? 2 : 0) + (flags & kMiiFavoriteBit ? 0 : 1);
        listed.emplace_back(pass, std::move(mii));
    }
    std::stable_sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<InstalledMii> miis;
    for (auto& entry : listed) miis.push_back(std::move(entry.second));
    return miis;
}
} // namespace

MiiDatabaseStatus InstalledMiis(const fs::path& nand_root, const SaveTitle& title) {
    // Only a database the catalog has: the game's device opens nothing else.
    // The same checks as AddMii and ImportDolphinMiis, without writing.
    MiiDatabaseStatus status;
    try {
        bool existing{};
        const auto catalog = ExistingCatalog(nand_root, title, existing);
        CheckMiiEntries(catalog);
        const auto file = Physical(nand_root / "data", kMiiDatabase);
        if (!catalog.entries.count(kMiiDatabase) || !IsFile(file)) return status;
        const auto database = ReadFile(file);
        if (!ValidMiiDatabase(database)) {
            status.state = MiiDatabaseState::Damaged;
            return status;
        }
        status.state = MiiDatabaseState::Ready;
        status.miis = ListedMiis(database, OnlineSlots(nand_root, catalog, title));
    } catch (const std::exception& error) {
        status = {MiiDatabaseState::Unusable, {}, error.what()};
    }
    return status;
}

std::vector<fs::path> InstalledMiiFiles(const fs::path& nand_root, const SaveTitle& title) {
    const auto data = nand_root / "data";
    return {nand_root / "metadata.txt", Physical(data, kMiiDatabase),
            Physical(data, Home(title.title_id) + '/' + kOnlineSave)};
}

std::uint64_t FriendCode(std::int32_t profile_id, std::uint32_t game_code) {
    // MATH_CalcCRC8 (polynomial 7, from 0) over both as little-endian words.
    std::uint8_t crc = 0;
    for (const std::uint32_t word : {std::uint32_t(profile_id), game_code})
        for (int n = 0; n < 4; ++n) {
            crc ^= std::uint8_t(word >> (8 * n));
            for (int bit = 0; bit < 8; ++bit) crc = std::uint8_t(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
        }
    return std::uint64_t(crc & 0x7F) << 32 | std::uint32_t(profile_id);
}

std::string FriendCodeText(std::uint64_t friend_code) {
    char text[32];
    std::snprintf(text, sizeof text, "%06llu %06llu", static_cast<unsigned long long>(friend_code / 1000000),
                  static_cast<unsigned long long>(friend_code % 1000000));
    return text;
}

MiiImportResult ImportDolphinMiis(const fs::path& source, const fs::path& nand_root, const SaveTitle& title,
                                  const fs::path& backup_parent) {
    std::error_code error;
    if (!IsFile(source) || fs::file_size(source, error) != kMiiDatabaseBytes)
        throw std::runtime_error(PathUtf8(source) + " is not a Wii Mii database.");
    const auto database = ReadFile(source);
    if (!ValidMiiDatabase(database))
        throw std::runtime_error("The Mii database " + PathUtf8(source) + " is damaged.");
    MiiImportResult result;
    result.names = MiiNames(database);
    if (result.names.empty())
        throw std::runtime_error("The Mii database " + PathUtf8(source)
                                 + " has no Miis. Create one in the Mii Channel first.");

    auto backing = OpenMiiBacking(nand_root, title);
    result.backup = WriteMiiDatabase(backing, title, database, backup_parent, "before-mii-import-");
    return result;
}

MiiImportResult AddMii(const std::string& record, const fs::path& nand_root, const SaveTitle& title,
                       const fs::path& backup_parent) {
    const auto* mii = reinterpret_cast<const unsigned char*>(record.data());
    if (record.size() != kMiiBytes || !MiiUsed(mii) || (!mii[kMiiNameOffset] && !mii[kMiiNameOffset + 1]))
        throw std::invalid_argument("Not a Mii record with a name and a create ID.");
    auto backing = OpenMiiBacking(nand_root, title);
    auto database = backing.database.empty() ? EmptyMiiDatabase() : backing.database;
    if (!ValidMiiDatabase(database))
        throw std::runtime_error("The game's Mii database is damaged. Import the Miis from Dolphin instead.");

    // The first free slot; a create ID already there would share its save slot.
    std::size_t free = kMiiSlots;
    for (std::size_t slot = 0; slot < kMiiSlots; ++slot) {
        const auto* other = MiiRecord(database, slot);
        if (!MiiUsed(other)) {
            if (free == kMiiSlots) free = slot;
        } else if (std::equal(other + kMiiCreateIdOffset, other + kMiiCreateIdOffset + kMiiCreateIdBytes,
                              mii + kMiiCreateIdOffset)) {
            throw MiiCreateIdTaken();
        }
    }
    if (free == kMiiSlots) throw std::runtime_error("The Mii database is full: it holds 100 Miis.");
    database.replace(4 + free * kMiiBytes, kMiiBytes, record);
    SetMiiCrc(database);

    MiiImportResult result;
    result.names = {MiiName(mii + kMiiNameOffset)};
    result.backup = WriteMiiDatabase(backing, title, database, backup_parent, "before-mii-create-");
    return result;
}

SidekickLineups DefaultSidekickLineups() {
    SidekickLineups lineups{};
    for (std::size_t captain = 0; captain < kCaptainCount; ++captain)
        for (std::size_t slot = 0; slot < kSidekickSlots; ++slot)
            lineups[captain][slot] = kDefaultLineups[captain][slot];
    return lineups;
}

SidekickSaveStatus InstalledSidekicks(const fs::path& nand_root, const SaveTitle& title) {
    SidekickSaveStatus status;
    try {
        const auto file = GameSaveFile(nand_root, title);
        if (file.empty()) return status;
        const auto lineups = SaveLineups(ReadFile(file));
        if (!lineups) {
            status.state = GameSaveState::Damaged;
            return status;
        }
        status.state = GameSaveState::Ready;
        status.lineups = *lineups;
    } catch (const std::exception& error) {
        status = {GameSaveState::Unusable, {}, error.what()};
    }
    return status;
}

fs::path WriteSidekicks(const fs::path& nand_root, const SaveTitle& title, const SidekickLineups& lineups,
                        const fs::path& backup_parent) {
    if (!ValidLineups(lineups)) throw std::invalid_argument("A sidekick is not one of the game's eight.");
    const auto file = GameSaveFile(physical::PrepareRoot(nand_root), title);
    auto save = SaveToChange(file);
    const auto current = SaveLineups(save);
    if (!current) throw std::runtime_error("The game cannot read its save, so it is left as it is.");
    if (*current == lineups) return {};
    for (std::size_t captain = 0; captain < kCaptainCount; ++captain)
        for (std::size_t slot = 0; slot < kSidekickSlots; ++slot)
            PutBig32(save, kLineupsOffset + (captain * kSidekickSlots + slot) * 4,
                     std::uint32_t(lineups[captain][slot]));
    return StoreGameSave(file, std::move(save), backup_parent, "before-sidekicks-");
}

bool operator==(const MatchSettings& a, const MatchSettings& b) {
    return a.skill == b.skill && a.goal_limit == b.goal_limit && a.time == b.time && a.goals == b.goals
        && a.best_of == b.best_of && a.stadium_cheat == b.stadium_cheat && a.player_cheat == b.player_cheat
        && a.powerup_cheat == b.powerup_cheat;
}

bool ValidMatchSettings(const MatchSettings& s) {
    // The choices of SHGameplayOptions (sSkillLevelOptions...) and the cheat enums (UserOptions.h).
    const bool time = s.time == 120 || s.time == 180 || s.time == 240 || s.time == 300;
    const bool series = s.best_of >= 1 && s.best_of <= 9 && s.best_of % 2 == 1;
    return s.skill >= 1 && s.skill <= 5 && time && s.goals >= 3 && s.goals <= 10 && series
        && s.stadium_cheat >= 0 && s.stadium_cheat <= 5 && s.player_cheat >= 0 && s.player_cheat <= 4
        && s.powerup_cheat >= 0 && s.powerup_cheat <= 11;
}

MatchSaveStatus InstalledMatchSettings(const fs::path& nand_root, const SaveTitle& title) {
    MatchSaveStatus status;
    try {
        const auto file = GameSaveFile(nand_root, title);
        if (file.empty()) return status;
        const auto save = ReadFile(file);
        if (!AcceptedSave(save)) {
            status.state = GameSaveState::Damaged;
            return status;
        }
        status.state = GameSaveState::Ready;
        for (std::size_t mode = 0; mode < kMatchModeCount; ++mode)
            status.modes[mode] = SaveMatch(save, MatchMode(mode));
    } catch (const std::exception& error) {
        status = {GameSaveState::Unusable, {}, error.what()};
    }
    return status;
}

fs::path WriteMatchSettings(const fs::path& nand_root, const SaveTitle& title, MatchMode mode,
                            const MatchSettings& settings, const fs::path& backup_parent) {
    if (!ValidMatchSettings(settings))
        throw std::invalid_argument("A match setting is not one the game's settings screen offers.");
    const auto file = GameSaveFile(physical::PrepareRoot(nand_root), title);
    auto save = SaveToChange(file);
    if (SaveMatch(save, mode) == settings) return {};
    PutMatch(save, mode, settings);
    return StoreGameSave(file, std::move(save), backup_parent, "before-match-settings-");
}
} // namespace mscharged::platform
