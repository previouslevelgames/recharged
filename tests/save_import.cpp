// Dolphin save import: the imported files must be what the game's own IOS
// filesystem device then opens, with the permissions the game gives its saves.
#include "platform/filesystem_device.h"
#include "platform/filesystem_physical.h"
#include "platform/game_release.h"
#include "platform/ios_device.h"
#include "platform/mii_maker.h"
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

// The Miis of a database the game loads; any other state fails the check.
std::vector<std::string> Miis(const fs::path& nand) {
    const auto status = InstalledMiis(nand, kTitle);
    Check(status.state == MiiDatabaseState::Ready && status.error.empty(), "Mii database not ready");
    return status.names;
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
    Check(InstalledMiis(nand, kTitle).state == MiiDatabaseState::Missing, "Mii database before any import");
    const std::vector<std::string> names{"Mario", "Lu\xE2\x98\x86gi\xF0\x9F\x98\x80"};
    auto result = ImportDolphinMiis(source, nand, kTitle, backups);
    Check(result.names == names, "Imported Mii names");
    Check(!result.backup.empty() && fs::exists(result.backup / "metadata.txt")
              && !fs::exists(result.backup / "RFL_DB.dat"),
          "First Mii import backup");
    Check(Miis(nand) == names, "Installed Miis after the import");
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
    Check(Miis(nand) == result.names, "Installed Miis after the second import");

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
    Check(Bytes(nand / "metadata.txt") == before && Miis(nand) == result.names,
          "Rejected Mii import changed the NAND");

    // A new save folder gets the device's layout plus the Miis.
    const auto fresh = root / "fresh-miis";
    Put(source, miis);
    result = ImportDolphinMiis(source, fresh, kTitle, backups);
    Check(result.backup.empty() && Miis(fresh) == names, "Mii import into a new save folder");
    InitializeNativeFilesystem({fresh, kTitle.title_id, kTitle.uid, kTitle.gid});
    Check(IosRead(kMiiPath, kMiiDatabaseBytes + 1) == miis, "New save folder's Mii database differs");
    ShutdownNativeFilesystem();
    // A database the catalog does not have is not one the game sees.
    const auto orphan = root / "orphan-miis";
    Put(orphan / "data" / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat", miis);
    const auto orphaned = InstalledMiis(orphan, kTitle);
    Check(orphaned.state == MiiDatabaseState::Unusable && !orphaned.error.empty() && orphaned.names.empty(),
          "A save folder without a catalog was not unusable");
    const auto other = InstalledMiis(nand, {kTitle.title_id + 1, kTitle.uid, kTitle.gid});
    Check(other.state == MiiDatabaseState::Unusable, "Another game's save folder was not unusable");
}

// A new Mii: into a save folder without Miis (a database formatted as the
// Mii library formats one), beside imported Miis, and never into a damaged
// or full database or twice.
void RunAddMii(const fs::path& root) {
    const std::int64_t now = 1760000000;
    const auto mario = MakeRandomMii("Mario", 1, now), luigi = MakeRandomMii("Luigi", 2, now);
    const auto fresh = root / "new-mii", backups = root / "backups";
    auto result = AddMii(mario, fresh, kTitle, backups);
    Check(result.backup.empty() && result.names == std::vector<std::string>{"Mario"}, "First new Mii");
    Check(Miis(fresh) == result.names, "New Mii not installed");
    InitializeNativeFilesystem({fresh, kTitle.title_id, kTitle.uid, kTitle.gid});
    const auto database = IosRead(kMiiPath, kMiiDatabaseBytes + 1);
    ShutdownNativeFilesystem();
    Check(database.size() == kMiiDatabaseBytes && MiiCrc(database) == 0, "New database fails the Mii library's CRC");
    Check(database.compare(4, 74, mario) == 0, "New Mii not in the first slot");
    Check(database.compare(0x1CEC, 4, std::string("\x80\0\0\0", 4)) == 0
              && database.compare(0x1D00, 8, "RNHD\xFF\xFF\xFF\xFF") == 0,
          "New database header differs from a formatted one");
    bool links = true;
    for (std::size_t entry = 0; entry < 10000; ++entry)
        links = links && database.compare(0x1D08 + entry * 12, 12, std::string(8, '\0') + "\x7F\xFF\x7F\xFF") == 0;
    Check(links && database.find_first_not_of('\0', kMiiChecked) == std::string::npos,
          "New database's hidden Miis differ from a formatted one");

    result = AddMii(luigi, fresh, kTitle, backups);
    Check(Miis(fresh) == std::vector<std::string>{"Mario", "Luigi"}
              && Bytes(result.backup / "RFL_DB.dat") == database,
          "Second new Mii, and the previous database's backup");
    const auto before = Bytes(fresh / "metadata.txt");
    bool taken{};
    try { AddMii(luigi, fresh, kTitle, backups); } catch (const MiiCreateIdTaken&) { taken = true; }
    Check(taken, "The same Mii was added twice");
    Reject([&] { AddMii(luigi.substr(1), fresh, kTitle, backups); }, "A short record was added");
    Check(Bytes(fresh / "metadata.txt") == before && Miis(fresh).size() == 2,
          "A rejected new Mii changed the save folder");
    // Backups made in the same second each keep their own state.
    const auto third = AddMii(MakeRandomMii("Toad", 4, now), fresh, kTitle, backups);
    Check(third.backup != result.backup && Bytes(result.backup / "RFL_DB.dat") == database,
          "A backup was overwritten by the next one");

    // Beside Dolphin's Miis: the first free slot.
    const auto imported = root / "nand";
    Check(Miis(imported) == std::vector<std::string>{"Luigi"}, "Imported Miis before a new one");
    AddMii(MakeRandomMii("Peach", 3, now), imported, kTitle, backups);
    Check(Miis(imported) == std::vector<std::string>{"Peach", "Luigi"}, "New Mii beside imported");

    // A full database, and a damaged one, take no new Mii.
    const auto full = root / "full-miis";
    std::vector<std::pair<std::size_t, std::u16string>> hundred;
    for (std::size_t slot = 0; slot < 100; ++slot) hundred.push_back({slot, u"Mii"});
    const auto source = DolphinMiiDatabase(root / "dolphin-user");
    Put(source, MiiDatabase(hundred));
    ImportDolphinMiis(source, full, kTitle, backups);
    Check(InstalledMiis(full, kTitle).Full(), "A database of 100 Miis is not full");
    Reject([&] { AddMii(mario, full, kTitle, backups); }, "A Mii was added to a full database");
    auto damaged = Bytes(source);
    damaged[0x100] ^= 1;
    namespace physical = mscharged::platform::filesystem_detail;
    auto stored = full / "data";
    for (const char* name : {"shared2", "menu", "FaceLib", "RFL_DB.dat"}) stored /= physical::Component(name);
    Put(stored, damaged);
    Check(InstalledMiis(full, kTitle).state == MiiDatabaseState::Damaged, "A damaged database was not damaged");
    Reject([&] { AddMii(mario, full, kTitle, backups); }, "A Mii was added to a damaged database");
    // Dolphin's Miis replace a damaged database, which is backed up.
    Put(source, MiiDatabase({{0, u"Daisy"}}));
    Check(Bytes(ImportDolphinMiis(source, full, kTitle, backups).backup / "RFL_DB.dat") == damaged
              && Miis(full) == std::vector<std::string>{"Daisy"},
          "Dolphin's Miis did not replace a damaged database");

    // A database the catalog has but the disk lost is missing: a new one replaces it.
    fs::remove(stored);
    Check(InstalledMiis(full, kTitle).state == MiiDatabaseState::Missing, "A lost database was not missing");
    result = AddMii(mario, full, kTitle, backups);
    Check(Miis(full) == std::vector<std::string>{"Mario"} && !fs::exists(result.backup / "RFL_DB.dat"),
          "A new Mii did not replace a lost database");
}

// The usual CRC-32, table-free (the game's nlChecksum32 uses its table).
std::uint32_t Crc32(const std::string& bytes, std::size_t begin) {
    std::uint32_t crc = ~0u;
    for (std::size_t n = begin; n < bytes.size(); ++n)
        for (int bit = 0; bit < 8; ++bit)
            crc = ((crc ^ (static_cast<unsigned char>(bytes[n]) >> bit)) & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    return ~crc;
}
void PutWord(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t n = 0; n < 4; ++n) bytes[offset + n] = char(value >> (24 - 8 * n));
}
std::uint32_t Word(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t n = 0; n < 4; ++n) value = value << 8 | static_cast<unsigned char>(bytes[offset + n]);
    return value;
}
// A game save as the game writes it: 35586 bytes of data (size and checksum
// first) in a 35616-byte file, with these lineups after the 0x80 bytes of
// user settings.
std::string GameSave(const SidekickLineups& lineups) {
    auto save = Pattern(5, 35616);
    PutWord(save, 0, 35586);
    for (std::size_t captain = 0; captain < kCaptainCount; ++captain)
        for (std::size_t slot = 0; slot < kSidekickSlots; ++slot)
            PutWord(save, 0x88 + (captain * kSidekickSlots + slot) * 4, std::uint32_t(lineups[captain][slot]));
    PutWord(save, 4, Crc32(save, 8));
    return save;
}

// Captains' lineups in the game save: what the launcher writes is what the
// game's device then reads, with a checksum the game accepts and nothing
// else changed; saves the game would reject are left alone.
void RunSidekicks(const fs::path& root) {
    Check(Crc32("123456789", 0) == 0xCBF43926u, "Test CRC-32 differs from the usual one");
    const auto defaults = DefaultSidekickLineups();
    Check(defaults[0] == SidekickLineup{1, 1, 7} && defaults[3] == SidekickLineup{0, 0, 0}
              && defaults[11] == SidekickLineup{6, 0, 0},
          "Default lineups differ from the game's");
    const auto nand = root / "sidekicks", backups = root / "sidekick-backups", source = root / "sidekick-source";
    fs::remove_all(nand);
    Check(InstalledSidekicks(nand, kTitle).state == SidekickSaveState::Missing, "Lineups before any save");
    Reject([&] { WriteSidekicks(nand, kTitle, defaults, backups); }, "Lineups written without a save");

    const auto save = GameSave(defaults);
    Put(source / "Strikers2", save);
    ImportDolphinSave(source, nand, kTitle, "Strikers2", backups);
    auto status = InstalledSidekicks(nand, kTitle);
    Check(status.state == SidekickSaveState::Ready && status.lineups == defaults, "Lineups of the save not read");

    // Mario with Boo, Toad and Dry Bones; Petey Piranha with three Shy Guys.
    auto lineups = defaults;
    lineups[0] = {4, 0, 5};
    lineups[11] = {7, 7, 7};
    const auto catalog = Bytes(nand / "metadata.txt");
    const auto backup = WriteSidekicks(nand, kTitle, lineups, backups);
    Check(!backup.empty() && Bytes(backup / "Strikers2") == save, "The save was not backed up");
    Check(Bytes(nand / "metadata.txt") == catalog, "Writing lineups changed the catalog");
    InitializeNativeFilesystem({nand, kTitle.title_id, kTitle.uid, kTitle.gid});
    const auto written = IosRead(kHome + "/Strikers2");
    ShutdownNativeFilesystem();
    Check(written.size() == save.size() && Word(written, 0) == 35586 && Word(written, 4) == Crc32(written, 8),
          "The written save fails the game's checks");
    Check(written.compare(0x88, 12, std::string("\0\0\0\4\0\0\0\0\0\0\0\5", 12)) == 0
              && written.compare(0x88 + 11 * 12, 12, std::string("\0\0\0\7\0\0\0\7\0\0\0\7", 12)) == 0,
          "Lineups not stored as the game reads them");
    auto unchanged = written;
    unchanged.replace(4, 4, save, 4, 4);
    unchanged.replace(0x88, 0x90, save, 0x88, 0x90);
    Check(unchanged == save, "Writing lineups changed more than the lineups and the checksum");
    status = InstalledSidekicks(nand, kTitle);
    Check(status.state == SidekickSaveState::Ready && status.lineups == lineups, "Written lineups not read back");

    // The same lineups again write nothing; no backup without a folder for it.
    Check(WriteSidekicks(nand, kTitle, lineups, backups).empty(), "Unchanged lineups were written");
    lineups[5] = {2, 2, 2};
    Check(WriteSidekicks(nand, kTitle, lineups, {}).empty() && InstalledSidekicks(nand, kTitle).lineups == lineups,
          "Lineups not written without a backup");

    // Sidekicks that do not exist, and saves the game rejects, change nothing.
    auto wrong = lineups;
    wrong[2][1] = 8;
    Reject([&] { WriteSidekicks(nand, kTitle, wrong, backups); }, "A ninth sidekick was written");
    wrong[2][1] = -1;
    Reject([&] { WriteSidekicks(nand, kTitle, wrong, backups); }, "A negative sidekick was written");
    namespace physical = mscharged::platform::filesystem_detail;
    auto stored = nand / "data";
    for (const char* name : {"title", "00010000", "52345145", "data", "Strikers2"}) stored /= physical::Component(name);
    const auto good = Bytes(stored);
    auto damaged = good;
    damaged[0x2000] ^= 1;
    Put(stored, damaged);
    Check(InstalledSidekicks(nand, kTitle).state == SidekickSaveState::Damaged, "A wrong checksum was not damaged");
    Reject([&] { WriteSidekicks(nand, kTitle, defaults, backups); }, "Lineups written into a damaged save");
    Check(Bytes(stored) == damaged, "A damaged save was changed");
    auto resized = good;
    PutWord(resized, 0, 35616 + 1);
    PutWord(resized, 4, Crc32(resized, 8));
    Put(stored, resized);
    Check(InstalledSidekicks(nand, kTitle).state == SidekickSaveState::Damaged, "A wrong size was not damaged");
    auto ninth = GameSave(defaults);
    PutWord(ninth, 0x88 + 4, 8);
    PutWord(ninth, 4, Crc32(ninth, 8));
    Put(stored, ninth);
    Check(InstalledSidekicks(nand, kTitle).state == SidekickSaveState::Damaged, "A ninth sidekick was read");
    Put(stored, good);
    Check(InstalledSidekicks(nand, {kTitle.title_id + 1, kTitle.uid, kTitle.gid}).state
              == SidekickSaveState::Unusable,
          "Another game's save folder was read");

    // The European save sits in its own title (TitleSaveFolder's R4QP): the
    // same layout, and the USA save stays as it is.
    constexpr SaveTitle kEurope{0x0001000052345150ULL, 0x1001, 0x3031};
    const auto europe = mscharged::TitleSaveFolder(nand, kEurope.title_id);
    Check(InstalledSidekicks(europe, kEurope).state == SidekickSaveState::Missing, "European lineups before a save");
    ImportDolphinSave(source, europe, kEurope, "Strikers2", backups);
    auto european = defaults;
    european[8] = {3, 3, 3};
    WriteSidekicks(europe, kEurope, european, backups);
    Check(InstalledSidekicks(europe, kEurope).lineups == european && InstalledSidekicks(nand, kTitle).lineups == lineups,
          "European lineups not kept apart from the USA ones");
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
        RunAddMii(fs::path(argv[1]));
        RunSidekicks(fs::path(argv[1]));
        RunFind(fs::path(argv[1]));
        std::cout << "save_import: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "save_import: " << e.what() << '\n';
        return 1;
    }
}
