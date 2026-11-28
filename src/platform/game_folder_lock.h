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
// starts one). It takes no lock.
bool GameFolderInUse(const std::filesystem::path& data_folder);

// A game that only plays a match back (a Watch link, a replay) while another
// game holds the folder watches beside it from a folder of its own,
// <data>/watching/<n>: nand/ holds a copy of the title's save folder (its
// data folder and catalog, as `title_save_folder` has them), native-rtc.bin
// a copy of the clock. The watcher keeps that folder's lock until it ends.
// Folders of watchers that ended are removed first. Throws with a readable
// message when no folder can be made.
std::filesystem::path PrepareWatcherFolder(const std::filesystem::path& data_folder,
                                           const std::filesystem::path& title_save_folder);
// Removes the folders of watchers that ended (a game that takes the folder
// does it at its start); never throws.
void RemoveEndedWatchers(const std::filesystem::path& data_folder);

} // namespace mscharged::platform
