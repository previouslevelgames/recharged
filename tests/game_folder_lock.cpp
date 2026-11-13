#include "platform/game_folder_lock.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
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
        Check(LockGameFolder(root / "a"), "The first game of a folder takes it");
        // A second open of the lock is another holder, as a second game's is.
        Check(!LockGameFolder(root / "a"), "A second game of the same folder is refused");
        Check(LockGameFolder(root / "b"), "A game of another folder is not stopped");
        Check(LockGameFolder(root / "missing"), "A folder without a lock file to make does not stop the game");
        std::cout << "Game folder lock checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Game folder lock test failed: " << error.what() << "; evidence: " << root << '\n';
        return 1;
    }
}
