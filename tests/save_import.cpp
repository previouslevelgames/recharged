// Dolphin save import: the imported files must be what the game's own IOS
// filesystem device then opens, with the permissions the game gives its saves.
#include "platform/filesystem_device.h"
#include "platform/ios_device.h"
#include "platform/save_import.h"
#include <revolution/ipc.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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
std::string IosRead(const std::string& path) {
    const auto fd = IOS_Open(path.c_str(), IPC_OPEN_READ);
    Check(fd >= 0, "Imported file does not open through the IOS device");
    std::string bytes(70000, '\0');
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
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: save_import_tests DATA_DIRECTORY");
        Run(fs::path(argv[1]));
        std::cout << "save_import: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "save_import: " << e.what() << '\n';
        return 1;
    }
}
