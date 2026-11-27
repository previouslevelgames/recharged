#include "platform/game_folder_lock.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using mscharged::platform::GameFolderInUse;
using mscharged::platform::LockGameFolder;
unsigned checks = 0;

void Check(bool yes, const char* message) {
    ++checks;
    if (!yes) throw std::runtime_error(message);
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
        std::cout << "Game folder lock checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Game folder lock test failed: " << error.what() << "; evidence: " << root << '\n';
        return 1;
    }
}
