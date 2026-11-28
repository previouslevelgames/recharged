#include "platform/game_folder_lock.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using mscharged::platform::GameFolderInUse;
using mscharged::platform::LockGameFolder;
using mscharged::platform::PrepareWatcherFolder;
using mscharged::platform::RemoveEndedWatchers;
unsigned checks = 0;

void Check(bool yes, const char* message) {
    ++checks;
    if (!yes) throw std::runtime_error(message);
}

void Write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) throw std::runtime_error("The test cannot write " + path.string());
}

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

int main() {
    const auto root = fs::temp_directory_path() /
        ("charged-game-folder-lock-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root / "a");
        fs::create_directories(root / "b");
        fs::create_directories(root / "c");
        Check(!GameFolderInUse(root / "a"), "A folder no game ran from is not in use");
        Check(LockGameFolder(root / "a"), "The first game of a folder takes it");
        // A second open of the lock is another holder, as a second game's is.
        Check(GameFolderInUse(root / "a"), "The launcher sees the folder's game");
        Check(!LockGameFolder(root / "a"), "A second game of the same folder is refused");
        Check(GameFolderInUse(root / "a"), "Asking leaves the game its lock");
        Check(LockGameFolder(root / "b"), "A game of another folder is not stopped");
        // A lock file left by a game that ended holds nothing.
        std::FILE* old = std::fopen((root / "c" / ".game.lock").string().c_str(), "w");
        Check(old != nullptr, "The test makes an old lock file");
        std::fclose(old);
        Check(!GameFolderInUse(root / "c"), "An old lock file is no game");
        Check(LockGameFolder(root / "c"), "Asking takes no lock");
        Check(!GameFolderInUse(root / "missing"), "A missing folder is not in use");
        Check(LockGameFolder(root / "missing"), "A folder without a lock file to make does not stop the game");

        // A watcher beside the game of folder a: a copy of the save and the clock.
        const auto save = root / "save";
        fs::create_directories(save / "data" / "title" / "00010000");
        fs::create_directories(save / "backups" / "old");
        Write(save / "metadata.txt", "catalog");
        Write(save / "data" / "title" / "00010000" / "Strikers2", "save");
        Write(root / "a" / "native-rtc.bin", "clock");
        const auto first = PrepareWatcherFolder(root / "a", save);
        Check(first == root / "a" / "watching" / "1", "The first watcher takes watching/1");
        Check(Read(first / "nand" / "metadata.txt") == "catalog", "The watcher has the save's catalog");
        Check(Read(first / "nand" / "data" / "title" / "00010000" / "Strikers2") == "save",
              "The watcher has the save's files");
        Check(!fs::exists(first / "nand" / "backups"), "The save's backups stay where they are");
        Check(Read(first / "native-rtc.bin") == "clock", "The watcher has the clock");
        Check(GameFolderInUse(first), "The watcher keeps its folder");
        Check(GameFolderInUse(root / "a"), "The game keeps the folder");
        const auto second = PrepareWatcherFolder(root / "a", save);
        Check(second == root / "a" / "watching" / "2", "A second watcher takes the next folder");
        Check(fs::exists(first), "A running watcher's folder stays");
        // One that ended left its folder without a holder.
        fs::create_directories(root / "a" / "watching" / "7" / "nand");
        Write(root / "a" / "watching" / "7" / ".game.lock", "");
        RemoveEndedWatchers(root / "a");
        Check(!fs::exists(root / "a" / "watching" / "7"), "An ended watcher's folder goes");
        Check(fs::exists(first) && fs::exists(second), "Running watchers keep theirs");
        // A first start: no save yet, no clock.
        const auto fresh = PrepareWatcherFolder(root / "b", root / "no-save");
        Check(fs::is_directory(fresh / "nand") && fs::is_empty(fresh / "nand"), "Without a save the copy is empty");
        Check(!fs::exists(fresh / "native-rtc.bin"), "Without a clock none is copied");
        RemoveEndedWatchers(root / "c");
        Check(!fs::exists(root / "c" / "watching"), "A folder without watchers gets none");
        std::cout << "Game folder lock checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Game folder lock test failed: " << error.what() << "; evidence: " << root << '\n';
        return 1;
    }
}
