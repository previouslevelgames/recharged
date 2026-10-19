// Dolphin save import: the imported files must be what the game's own IOS
// filesystem device then opens, with the permissions the game gives its saves.
#include "platform/filesystem_device.h"
#include "platform/game_release.h"
#include "platform/ios_device.h"
#include "platform/save_import.h"
#include <revolution/ipc.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace mscharged::platform;
unsigned checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template <class F> void Reject(F f, const char* message) {
    bool failed{};
    try { f(); } catch (const std::exception&) { failed = true; }
    Check(failed, message);
}
constexpr SaveTitle kTitle{0x0001000052345145ULL, 0x1001, 0x3031};
const std::string kHome = "/title/00010000/52345145/data";

void Put(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}
std::string Bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
std::string Pattern(unsigned seed, std::size_t size) {
    std::string bytes(size, '\0');
    for (std::size_t n = 0; n < size; ++n) bytes[n] = char(n * 31 + seed * 7);
    bytes[0] = 0; bytes[1] = '\r'; bytes[2] = '\n'; bytes[3] = 26;
    return bytes;
}
std::string IosRead(const std::string& path, std::size_t capacity = 70000) {
    const auto fd = IOS_Open(path.c_str(), IPC_OPEN_READ);
    Check(fd >= 0, "Imported file does not open through the IOS device");
    std::string bytes(capacity, '\0');
    const auto count = IOS_Read(fd, bytes.data(), u32(bytes.size()));
    Check(count >= 0 && IOS_Close(fd) == 0, "Imported file does not read through the IOS device");
    bytes.resize(std::size_t(count));
    return bytes;
}
struct Fields { u32 uid; u16 gid; char path[64]; u8 owner, group, other, attr, unused[2]; };
Fields Attr(const std::string& path) {
    const auto device = IOS_Open("/dev/fs", IPC_OPEN_NONE);
    Check(device >= 0, "FS endpoint did not open");
    char name[64]{};
    std::memcpy(name, path.c_str(), path.size());
    Fields fields{};
    Check(IOS_Ioctl(device, 6, name, sizeof name, &fields, sizeof fields) == 0 && IOS_Close(device) == 0,
          "Imported file attributes are unavailable");
    return fields;
}

void Run(const fs::path& root) {
    fs::remove_all(root);
    const auto dolphin = root / "dolphin-user";
    const auto source = DolphinSaveFolder(dolphin, kTitle.title_id);
    Check(source == dolphin / "Wii" / "title" / "00010000" / "52345145" / "data", "Dolphin save folder layout");
    const auto save = Pattern(1, 35616), banner = Pattern(2, 61600);
    Put(source / "Strikers2", save);
    Put(source / "banner.bin", banner);

    // Every folder the player may choose leads to the data folder.
    for (const auto& chosen : {source, source / "Strikers2", source.parent_path(), dolphin / "Wii", dolphin})
        Check(ResolveDolphinSave(chosen, kTitle.title_id, "Strikers2") == source, "Chosen folder not resolved");
    Check(ResolveDolphinSave(root, kTitle.title_id, "Strikers2").empty(), "Unrelated folder resolved");

    // A fresh NAND gets the device's initial layout plus the save.
    const auto nand = root / "nand", backups = root / "backups";
    auto result = ImportDolphinSave(source, nand, kTitle, "Strikers2", backups);
    Check(result.backup.empty() && result.files == std::vector<std::string>{"Strikers2", "banner.bin"},
          "Fresh import result");
    InitializeNativeFilesystem({nand, kTitle.title_id, kTitle.uid, kTitle.gid});
    Check(IosRead(kHome + "/Strikers2") == save && IosRead(kHome + "/banner.bin") == banner,
          "Fresh import bytes differ through the IOS device");
    const auto attr = Attr(kHome + "/Strikers2");
    Check(attr.uid == kTitle.uid && attr.gid == kTitle.gid && attr.owner == 3 && !attr.group && !attr.other,
          "Imported save permissions differ from the game's own");
    ShutdownNativeFilesystem();

    // An existing NAND keeps its catalog; replaced files are backed up first.
    const auto changed = Pattern(3, 35616);
    Put(source / "Strikers2", changed);
    fs::remove(source / "banner.bin");
    result = ImportDolphinSave(source, nand, kTitle, "Strikers2", backups);
    Check(!result.backup.empty() && Bytes(result.backup / "Strikers2") == save
              && fs::exists(result.backup / "metadata.txt") && !fs::exists(result.backup / "banner.bin"),
          "Previous save not backed up");
    InitializeNativeFilesystem({nand, kTitle.title_id, kTitle.uid, kTitle.gid});
    Check(IosRead(kHome + "/Strikers2") == changed && IosRead(kHome + "/banner.bin") == banner,
          "Second import did not replace only the given files");
    ShutdownNativeFilesystem();

    // Unusable sources and backings change nothing.
    const auto before = Bytes(nand / "metadata.txt");
    fs::remove(source / "Strikers2");
    Put(source / "other", "x");
    Reject([&] { ImportDolphinSave(source, nand, kTitle, "Strikers2", backups); }, "Missing save accepted");
    Put(source / "Strikers2", save);
    Reject([&] { ImportDolphinSave(source, nand, {kTitle.title_id + 1, kTitle.uid, kTitle.gid}, "Strikers2",
                                   backups); },
           "Another game's NAND accepted");
    Put(source / "a-name-too-long", "x");
    Reject([&] { ImportDolphinSave(source, nand, kTitle, "Strikers2", backups); }, "Invalid Wii name accepted");
    Check(Bytes(nand / "metadata.txt") == before, "Rejected import changed the catalog");
    fs::remove(source / "a-name-too-long");
    const auto orphan = root / "orphan";
    Put(orphan / "data" / "stray", "x");
    Reject([&] { ImportDolphinSave(source, orphan, kTitle, "Strikers2", backups); },
           "Backing without a catalog accepted");
}

// A European save lives beside the USA one: in the save folder's R4QP
// subfolder (TitleSaveFolder), under the European title, and leaves the USA
// catalog alone. Both European revisions share that title.
void RunEurope(const fs::path& root) {
    constexpr SaveTitle kEurope{0x0001000052345150ULL, 0x1001, 0x3031};
    const std::string home = "/title/00010000/52345150/data";
    const auto source = DolphinSaveFolder(root / "dolphin-user", kEurope.title_id);
    Check(source == root / "dolphin-user" / "Wii" / "title" / "00010000" / "52345150" / "data",
          "European Dolphin save folder layout");
    const auto save = Pattern(4, 35616);
    Put(source / "Strikers2", save);
    const auto nand = root / "nand", europe = mscharged::TitleSaveFolder(nand, kEurope.title_id);
    Check(europe == nand / "R4QP" && mscharged::TitleSaveFolder(nand, kTitle.title_id) == nand,
          "Europe keeps its storage in R4QP; the USA title keeps the save folder");
    const auto usa_catalog = Bytes(nand / "metadata.txt");
    Reject([&] { ImportDolphinSave(source, nand, kEurope, "Strikers2", root / "backups"); },
           "A European save was accepted into the USA catalog");
    Reject([&] { InitializeNativeFilesystem({nand, kEurope.title_id, kEurope.uid, kEurope.gid}); },
           "The USA storage opened under the European title");
    const auto result = ImportDolphinSave(source, europe, kEurope, "Strikers2", europe / "backups");
    Check(result.backup.empty() && result.files == std::vector<std::string>{"Strikers2"}, "European import result");
    InitializeNativeFilesystem({europe, kEurope.title_id, kEurope.uid, kEurope.gid});
    Check(IosRead(home + "/Strikers2") == save, "European save differs through the IOS device");
    const auto attr = Attr(home + "/Strikers2");
    Check(attr.uid == kEurope.uid && attr.gid == kEurope.gid && attr.owner == 3, "European save permissions");
    ShutdownNativeFilesystem();
    // The USA storage still opens with its own save, the European folder inside it.
    InitializeNativeFilesystem({nand, kTitle.title_id, kTitle.uid, kTitle.gid});
    Check(IosRead(kHome + "/Strikers2") == Pattern(3, 35616), "The USA save no longer opens beside Europe's");
    ShutdownNativeFilesystem();
    Check(Bytes(nand / "metadata.txt") == usa_catalog, "The European import changed the USA catalog");
}

// A Mii database as the Mii library writes it (RFL_Database.c): "RNOD", 100
// Miis of 0x4A bytes (name at 0x2, create ID at 0x18), the hidden database
// at 0x1D00 and a CRC over the first 0x1F1E0 bytes that comes out 0.
constexpr std::size_t kMiiDatabaseBytes = 0xBE6C0, kMiiChecked = 0x1F1E0;
const std::string kMiiPath = "/shared2/menu/FaceLib/RFL_DB.dat";
std::uint16_t MiiCrc(const std::string& bytes) {
    std::uint16_t crc = 0;
    for (std::size_t n = 0; n < kMiiChecked; ++n) {
        auto data = static_cast<unsigned char>(bytes[n]);
        for (int bit = 0; bit < 8; ++bit, data <<= 1) {
            crc = std::uint16_t(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
            if (data & 0x80) crc ^= 1;
        }
    }
    return crc;
}
std::string MiiDatabase(const std::vector<std::pair<std::size_t, std::u16string>>& miis) {
    std::string bytes(kMiiDatabaseBytes, '\0');
    bytes.replace(0, 4, "RNOD");
    bytes.replace(0x1D00, 4, "RNHD");
    for (const auto& [slot, name] : miis) {
        const auto mii = 4 + slot * 0x4A;
        for (std::size_t n = 0; n < name.size(); ++n) {
            bytes[mii + 2 + 2 * n] = char(name[n] >> 8);
            bytes[mii + 3 + 2 * n] = char(name[n]);
        }
        bytes[mii + 0x18] = char(0x80 | slot);
        bytes[mii + 0x1F] = 0x5B;
    }
    // With the CRC field 0, the CRC of the rest is what the field must hold.
    const auto crc = MiiCrc(bytes);
    bytes[kMiiChecked - 2] = char(crc >> 8);
    bytes[kMiiChecked - 1] = char(crc);
    return bytes;
}

// Dolphin's Mii database: what the game's device opens afterwards, read and
// write for the game, and nothing changed when it is unusable.
void RunMiis(const fs::path& root) {
    const auto dolphin = root / "dolphin-user", source = DolphinMiiDatabase(dolphin);
    Check(source == dolphin / "Wii" / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat", "Dolphin Mii database layout");
    const auto miis = MiiDatabase({{0, u"Mario"}, {3, u"Lu☆gi\U0001F600"}});
    Check(MiiCrc(miis) == 0, "Test database fails the Mii library's CRC");
    Put(source, miis);
    for (const auto& chosen : {source, source.parent_path(), source.parent_path().parent_path(),
                               dolphin / "Wii" / "shared2", dolphin / "Wii", dolphin})
        Check(ResolveDolphinMiiDatabase(chosen) == source, "Chosen Mii folder not resolved");
    Check(ResolveDolphinMiiDatabase(root / "backups").empty(), "Unrelated folder resolved to a Mii database");

    // Into the NAND that has the save: the shared folders join its catalog.
    const auto nand = root / "nand", backups = root / "backups";
    Check(InstalledMiis(nand, kTitle).empty(), "Miis listed before any import");
    const std::vector<std::string> names{"Mario", "Lu\xE2\x98\x86gi\xF0\x9F\x98\x80"};
    auto result = ImportDolphinMiis(source, nand, kTitle, backups);
    Check(result.names == names, "Imported Mii names");
    Check(!result.backup.empty() && fs::exists(result.backup / "metadata.txt")
              && !fs::exists(result.backup / "RFL_DB.dat"),
          "First Mii import backup");
    Check(InstalledMiis(nand, kTitle) == names, "Installed Miis after the import");
    InitializeNativeFilesystem({nand, kTitle.title_id, kTitle.uid, kTitle.gid});
    Check(IosRead(kMiiPath, kMiiDatabaseBytes + 1) == miis, "Mii database differs through the IOS device");
    const auto fd = IOS_Open(kMiiPath.c_str(), IPC_OPEN_RW);
    Check(fd >= 0 && IOS_Close(fd) == 0, "The game cannot write the Mii database");
    const auto attr = Attr(kMiiPath);
    Check(!attr.uid && attr.owner == 3 && attr.group == 3 && attr.other == 3, "Mii database permissions");
    Check(IosRead(kHome + "/Strikers2") == Pattern(3, 35616), "The Mii import changed the save");
    ShutdownNativeFilesystem();

    // A second import replaces the database; the previous one is backed up.
    const auto changed = MiiDatabase({{3, u"Luigi"}});
    Put(source, changed);
    result = ImportDolphinMiis(source, nand, kTitle, backups);
    Check(result.names == std::vector<std::string>{"Luigi"} && Bytes(result.backup / "RFL_DB.dat") == miis,
          "Previous Mii database not backed up");
    Check(InstalledMiis(nand, kTitle) == result.names, "Installed Miis after the second import");

    // Unusable databases change nothing.
    const auto before = Bytes(nand / "metadata.txt");
    auto damaged = changed;
    damaged[0x100] ^= 1;
    Put(source, damaged);
    Reject([&] { ImportDolphinMiis(source, nand, kTitle, backups); }, "Damaged Mii database accepted");
    Put(source, MiiDatabase({}));
    Reject([&] { ImportDolphinMiis(source, nand, kTitle, backups); }, "Mii database without Miis accepted");
    Put(source, changed.substr(0, kMiiChecked));
    Reject([&] { ImportDolphinMiis(source, nand, kTitle, backups); }, "Short Mii database accepted");
    Check(Bytes(nand / "metadata.txt") == before && InstalledMiis(nand, kTitle) == result.names,
          "Rejected Mii import changed the NAND");

    // A new save folder gets the device's layout plus the Miis.
    const auto fresh = root / "fresh-miis";
    Put(source, miis);
    result = ImportDolphinMiis(source, fresh, kTitle, backups);
    Check(result.backup.empty() && InstalledMiis(fresh, kTitle) == names, "Mii import into a new save folder");
    InitializeNativeFilesystem({fresh, kTitle.title_id, kTitle.uid, kTitle.gid});
    Check(IosRead(kMiiPath, kMiiDatabaseBytes + 1) == miis, "New save folder's Mii database differs");
    ShutdownNativeFilesystem();
    // A database the catalog does not have is not one the game sees.
    const auto orphan = root / "orphan-miis";
    Put(orphan / "data" / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat", miis);
    Check(InstalledMiis(orphan, kTitle).empty(), "A Mii database outside the catalog was listed");
}

void SetEnvironment(const char* name, const fs::path& value) {
#ifdef _WIN32
    const std::wstring wide(name, name + std::strlen(name));
    _wputenv_s(wide.c_str(), value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// The launcher offers what FindDolphinSaves and FindDolphinMiiDatabases find
// in Dolphin's usual user directories, which the environment locates: here
// they all point below `root`. A folder two of them reach is listed once.
void RunFind(const fs::path& root) {
    const auto home = root / "find-home";
    fs::remove_all(home);
    std::vector<fs::path> users;
#if defined(_WIN32)
    // OneDrive's Documents is the profile's own here: one folder, two ways.
    SetEnvironment("USERPROFILE", home);
    SetEnvironment("OneDrive", home);
    SetEnvironment("APPDATA", home / "AppData");
    users = {home / "Documents" / "Dolphin Emulator", home / "AppData" / "Dolphin Emulator"};
#elif defined(__APPLE__)
    SetEnvironment("HOME", home);
    users = {home / "Library" / "Application Support" / "Dolphin"};
#else
    // XDG_DATA_HOME is ~/.local/share here: one folder, two ways.
    SetEnvironment("HOME", home);
    SetEnvironment("XDG_DATA_HOME", home / ".local" / "share");
    users = {home / ".local" / "share" / "dolphin-emu",
             home / ".var" / "app" / "org.DolphinEmu.dolphin-emu" / "data" / "dolphin-emu"};
#endif
    constexpr std::uint64_t kEurope = 0x0001000052345150ULL;
    Check(FindDolphinSaves(kTitle.title_id).empty() && FindDolphinMiiDatabases().empty(),
          "Dolphin data found where there is none");

    // A data folder only counts as a folder; a file of its name is not a save.
    Put(DolphinSaveFolder(users.front(), kTitle.title_id), "x");
    Check(FindDolphinSaves(kTitle.title_id).empty(), "A file was found as a Dolphin save folder");
    fs::remove(DolphinSaveFolder(users.front(), kTitle.title_id));

    fs::create_directories(DolphinSaveFolder(users.front(), kTitle.title_id));
    Put(DolphinMiiDatabase(users.front()), "x");
    Check(FindDolphinSaves(kTitle.title_id) == std::vector<fs::path>{DolphinSaveFolder(users.front(), kTitle.title_id)},
          "Dolphin save not found once");
    Check(FindDolphinSaves(kEurope).empty(), "A save of the other region was found");
    Check(FindDolphinMiiDatabases() == std::vector<fs::path>{DolphinMiiDatabase(users.front())},
          "Dolphin Mii database not found once");
    if (users.size() < 2) return;

    // Every user directory counts, in the order the launcher offers them.
    fs::create_directories(DolphinSaveFolder(users.back(), kTitle.title_id));
    fs::create_directories(DolphinSaveFolder(users.back(), kEurope));
    Check(FindDolphinSaves(kTitle.title_id)
              == std::vector<fs::path>{DolphinSaveFolder(users.front(), kTitle.title_id),
                                       DolphinSaveFolder(users.back(), kTitle.title_id)},
          "Dolphin saves of both user directories");
    Check(FindDolphinSaves(kEurope) == std::vector<fs::path>{DolphinSaveFolder(users.back(), kEurope)},
          "European Dolphin save");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: save_import_tests DATA_DIRECTORY");
        Run(fs::path(argv[1]));
        RunEurope(fs::path(argv[1]));
        RunMiis(fs::path(argv[1]));
        RunFind(fs::path(argv[1]));
        std::cout << "save_import: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "save_import: " << e.what() << '\n';
        return 1;
    }
}
