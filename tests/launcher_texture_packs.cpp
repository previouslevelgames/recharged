// The Extras page's texture packs (launcher/texture_pack_library.h): the
// settings strings for order and on/off, and dropped packs checked and copied
// in the background.

#include "launcher/texture_pack_library.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace
{
namespace fs = std::filesystem;
using mscharged::launcher::TexturePackLibrary;
using Kind = TexturePackLibrary::Event::Kind;

int failures = 0;

void Check(bool value, const std::string& message)
{
    if (!value)
    {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void Put(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// Polls (as the launcher does once a frame) until an event comes, or 10 s.
std::vector<TexturePackLibrary::Event> NextEvents(TexturePackLibrary& library, const fs::path& folder)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until)
    {
        auto events = library.Poll(folder, true);
        if (!events.empty()) return events;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return {};
}

// Polls until the folder was read.
void Settle(TexturePackLibrary& library, const fs::path& folder)
{
    library.Poll(folder, true); // starts the reading asked for
    for (int n = 0; n < 2000 && library.Scanning(); ++n)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        library.Poll(folder, true);
    }
}

void TestSettings()
{
    using mscharged::platform::TexturePack;
    std::vector<TexturePack> ordered(3);
    ordered[0].name = "A";
    ordered[1].name = "B";
    ordered[2].name = "C";
    const auto moved = mscharged::launcher::PackOrderAfterMove(ordered, 2, 0);
    Check(moved && *moved == mscharged::platform::JoinPackNames({"C", "A", "B"}), "A pack moved to the top");
    Check(!mscharged::launcher::PackOrderAfterMove(ordered, 1, 1) && !mscharged::launcher::PackOrderAfterMove(ordered, 3, 0),
          "Nothing moves in place or out of range");
    const auto off = mscharged::launcher::PacksOffWith("", "B", false);
    Check(off == mscharged::platform::JoinPackNames({"B"}), "A pack turned off");
    Check(mscharged::launcher::PacksOffWith(off, "B", true).empty(), "A pack turned on again");
}

void TestAdding(const fs::path& work)
{
    fs::remove_all(work);
    const fs::path folder = work / "texture_packs";
    const fs::path pack = work / "drops" / "Sharp Pack";
    Put(pack / "Characters" / "tex1_64x64_m_0123456789abcdef_14.png", "image");
    Put(work / "drops" / "game.iso", "not a pack");
    Put(work / "drops" / "Empty Folder" / "notes.txt", "no textures");

    TexturePackLibrary library;
    Check(!library.Add(work / "drops" / "game.iso", folder), "A file that is no folder or .zip is no pack");
    Check(library.Add(work / "drops" / "Empty Folder", folder), "A folder is checked in the background");
    auto events = NextEvents(library, folder);
    Check(events.size() == 1 && events[0].kind == Kind::NotAPack && events[0].source == work / "drops" / "Empty Folder",
          "A folder without textures is told back as no pack");

    Check(library.Add(pack, folder), "A pack is added");
    events = NextEvents(library, folder);
    if (events.empty() && library.Copying()) events = NextEvents(library, folder);
    Check(events.size() == 1 && events[0].kind == Kind::Added && events[0].pack == "Sharp Pack",
          "The pack is copied in");
    Check(fs::exists(folder / "Sharp Pack" / "Characters" / "tex1_64x64_m_0123456789abcdef_14.png")
              && !fs::exists(folder / ".Sharp Pack.adding"),
          "Its files are in the folder, nothing half copied");
    Settle(library, folder);
    Check(library.FolderExists() && library.Packs().size() == 1 && library.Packs()[0].name == "Sharp Pack",
          "The folder is read again after the pack came in");

    Check(library.Add(pack, folder), "The same pack again");
    events = NextEvents(library, folder);
    Check(events.size() == 1 && events[0].kind == Kind::FolderHasPack, "The folder has it already");
    Check(library.Add(folder / "Sharp Pack", folder), "A pack of the folder itself");
    events = NextEvents(library, folder);
    Check(events.size() == 1 && events[0].kind == Kind::AlreadyInFolder, "It is in the folder already");
}

void TestStoppedCopy(const fs::path& work)
{
    const fs::path pack = work / "drops" / "Big Pack";
    for (int n = 0; n < 20; ++n) Put(pack / ("tex1_64x64_000000000000000" + std::to_string(n % 10) + "_14.png"), "x");
    const fs::path folder = work / "stopped";
    std::atomic<bool> stop{true};
    Check(!mscharged::launcher::CopyTexturePack(pack, folder, stop).empty(), "A stopped copy fails");
    Check(!fs::exists(folder / "Big Pack") && !fs::exists(folder / ".Big Pack.adding"), "It leaves nothing behind");
}
} // namespace

int main(int argc, char** argv)
{
    const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "launcher-texture-packs";
    TestSettings();
    TestAdding(work);
    TestStoppedCopy(work);
    if (failures) std::cerr << failures << " texture pack check(s) failed\n";
    return failures ? 1 : 0;
}
