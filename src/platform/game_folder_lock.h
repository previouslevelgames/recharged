#pragma once

#include <filesystem>

namespace mscharged::platform {

// One game per game folder: two would share its save (data/nand), and a
// competitive set would get both of the player's games. Takes an exclusive
// lock on <data>/.game.lock that the process keeps until it ends (or execs:
// the launcher a game reopens holds none). False: another game holds it. A
// folder where no lock can be taken (read-only) does not stop the game.
bool LockGameFolder(const std::filesystem::path& data_folder);
// Whether another game holds the folder's lock now (the launcher asks before it
// starts one: a game from a Watch link while one runs). It takes no lock.
bool GameFolderInUse(const std::filesystem::path& data_folder);

} // namespace mscharged::platform
