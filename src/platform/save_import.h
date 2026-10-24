#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// Import a Wii save that Dolphin keeps in its emulated NAND. Dolphin stores
// the files of a title's data folder exactly as the game wrote them, so they
// are copied byte for byte into the port's NAND backing and its ownership
// catalog (filesystem_device.cpp) gains entries for new files. The Mii
// database is imported, or a new Mii added to it, the same way. The game
// must not be running: the catalog is only read at its start.
namespace mscharged::platform {
// Identity of the title whose save is imported; the same values
// InitializeNativeFilesystemForDisc gives the game's storage.
struct SaveTitle {
    std::uint64_t title_id;
    std::uint32_t uid;
    std::uint16_t gid;
};

// The title's save folder below a Dolphin user directory:
// <user>/Wii/title/00010000/52345145/data for R4QE, .../52345150/data for R4QP.
std::filesystem::path DolphinSaveFolder(const std::filesystem::path& dolphin_user, std::uint64_t title_id);

// Dolphin save folders of this title that exist on this computer, in
// Dolphin's usual user directories (installed, Flatpak, legacy, Windows
// Documents/AppData, macOS Application Support).
std::vector<std::filesystem::path> FindDolphinSaves(std::uint64_t title_id);

// The title's save folder for what the player chose: a file in the data
// folder, the data folder itself, or a folder above it (title, Wii, the
// Dolphin user directory). Empty when no `required` file is found there.
std::filesystem::path ResolveDolphinSave(const std::filesystem::path& chosen, std::uint64_t title_id,
                                         const std::string& required);

struct SaveImportResult {
    std::vector<std::string> files;   // imported names, e.g. Strikers2, banner.bin
    std::filesystem::path backup;     // previous save, empty when there was none
};

// Copy every file of `source` (a Dolphin data folder) into the title's data
// folder of the NAND backing at `nand_root`. Files that are replaced, and the
// catalog, are first copied to a new folder `backup_parent`/<timestamp>.
// `required` must be among the files (the game's own save). Throws with a
// readable message when the source or the existing backing is not usable;
// nothing is changed then.
SaveImportResult ImportDolphinSave(const std::filesystem::path& source, const std::filesystem::path& nand_root,
                                   const SaveTitle& title, const std::string& required,
                                   const std::filesystem::path& backup_parent);

// The Miis. Every Wii game reads them from one database in the shared part of
// the NAND, /shared2/menu/FaceLib/RFL_DB.dat, and online play needs at least
// one. Dolphin keeps it below its user directory:
// <user>/Wii/shared2/menu/FaceLib/RFL_DB.dat.
std::filesystem::path DolphinMiiDatabase(const std::filesystem::path& dolphin_user);

// Dolphin Mii databases that exist on this computer, in the same user
// directories as FindDolphinSaves.
std::vector<std::filesystem::path> FindDolphinMiiDatabases();

// The Mii database for what the player chose: the file itself, or a folder
// from FaceLib up to the Dolphin user directory. Empty when none is there.
std::filesystem::path ResolveDolphinMiiDatabase(const std::filesystem::path& chosen);

// A Mii database holds at most this many Miis.
inline constexpr std::size_t kMiiDatabaseSlots = 100;

enum class MiiDatabaseState {
    Missing,  // none yet: AddMii and ImportDolphinMiis make one
    Ready,    // the game loads it
    Damaged,  // the catalog has one the game rejects (size, magic or CRC)
    Unusable, // the save folder cannot take one: `error` says why
};

struct MiiDatabaseStatus {
    MiiDatabaseState state = MiiDatabaseState::Missing;
    std::vector<std::string> names; // the Miis of a Ready database, in its order
    std::string error;              // Unusable only
    bool Full() const { return names.size() >= kMiiDatabaseSlots; }
};

// What the game finds as its Mii database in the NAND backing at
// `nand_root`: the database the catalog has (the device opens nothing else).
// Reads only; a Missing or Ready database can take a new Mii unless Full.
MiiDatabaseStatus InstalledMiis(const std::filesystem::path& nand_root, const SaveTitle& title);

struct MiiImportResult {
    std::vector<std::string> names;   // the imported Miis, UTF-8
    std::filesystem::path backup;     // previous catalog and database, empty for a new save folder
};

// Copy the Mii database `source` into the NAND backing at `nand_root`, after
// the checks the game's Mii library makes when it loads it. The previous
// database and the catalog are first copied to `backup_parent`/<timestamp>.
// Throws with a readable message when `source` is not a database with Miis or
// the backing is not usable; nothing is changed then.
MiiImportResult ImportDolphinMiis(const std::filesystem::path& source, const std::filesystem::path& nand_root,
                                  const SaveTitle& title, const std::filesystem::path& backup_parent);

// The database already has a Mii with the new one's create ID, which would
// share its save slot: make the Mii again (another create ID) and retry.
struct MiiCreateIdTaken : std::runtime_error {
    MiiCreateIdTaken() : std::runtime_error("The Mii database already has a Mii with this create ID.") {}
};

// Add one Mii (its 74-byte record, mii_maker.h) to the database of the NAND
// backing at `nand_root`, in its first free slot; a backing without a
// database gets a new one, as the Mii library formats it. `names` holds the
// new Mii's name. The previous database and the catalog are backed up as for
// an import. Throws MiiCreateIdTaken for a create ID already there, and a
// readable std::runtime_error for a damaged or full database or an unusable
// save folder (InstalledMiis tells these beforehand); nothing is changed then.
MiiImportResult AddMii(const std::string& record, const std::filesystem::path& nand_root, const SaveTitle& title,
                       const std::filesystem::path& backup_parent);

// The game save (Strikers2) as the launcher finds it, for the settings it
// changes there: the captains' lineups and the match settings.
enum class GameSaveState {
    Missing,  // no game save yet: the game makes it when it first saves
    Ready,    // the game loads it
    Damaged,  // the game rejects it (size or checksum), or it holds settings the game has not
    Unusable, // the save folder cannot be read: `error` says why
};

// The sidekicks each captain starts with. The game keeps one lineup per
// captain (GameInfoManager::mRulesTable): the sidekick screen shows it, and a
// choice made there replaces it. It travels in the game's own save,
// Strikers2, right after the user's settings, so the launcher changes it
// there and the game needs nothing of its own.
inline constexpr std::size_t kCaptainCount = 12, kSidekickCount = 8, kSidekickSlots = 3;
// The sidekicks (eSidekickID: 0 Toad ... 7 Shy Guy) by slot, the order the
// game fields them in.
using SidekickLineup = std::array<int, kSidekickSlots>;
// One lineup per captain, in eTeamID order (0 Mario ... 11 Petey Piranha).
using SidekickLineups = std::array<SidekickLineup, kCaptainCount>;

// The lineups a new save starts with (kDefaultRules, GameInfo.cpp).
SidekickLineups DefaultSidekickLineups();

struct SidekickSaveStatus {
    GameSaveState state = GameSaveState::Missing;
    SidekickLineups lineups{}; // Ready only
    std::string error;         // Unusable only
};

// The lineups in the game save of the NAND backing at `nand_root`: the save
// the catalog has (the device opens nothing else). Reads only.
SidekickSaveStatus InstalledSidekicks(const std::filesystem::path& nand_root, const SaveTitle& title);

// Write `lineups` into the game save of the NAND backing at `nand_root` and
// give it the checksum the game checks; nothing else in it changes. With a
// `backup_parent`, the save is first copied to `backup_parent`/<timestamp>,
// which is returned (empty without one, or when the save already holds these
// lineups). Throws std::invalid_argument for a sidekick outside 0..7, and a
// readable std::runtime_error when there is no save or the game would reject
// it (InstalledSidekicks tells these beforehand); nothing is changed then.
std::filesystem::path WriteSidekicks(const std::filesystem::path& nand_root, const SaveTitle& title,
                                     const SidekickLineups& lineups, const std::filesystem::path& backup_parent);

// The match settings and cheats of the game save (UserInfo, GameInfo.h):
// one set for matches on this console and one for online matches with
// friends (GameInfoManager::UseAltRules); ranked matches have fixed rules.
// The game's settings screen shows and replaces them.
enum class MatchMode { Offline, Friends };
inline constexpr std::size_t kMatchModeCount = 2;

// What the game's settings screen (SHGameplayOptions) sets of
// GameplaySettings, and CheatSettings; a new save's are these defaults.
// GameplaySettings also holds power-ups, Mega Strikes and skillshots for
// each team, which only Striker Challenges change: they are not here.
struct MatchSettings {
    int skill = 1;           // 1 Rookie, 2 Professional, 3 Superstar, 4 Legend, 5 Megastriker
    bool goal_limit = false; // first to `goals`, else the clock runs `time`
    int time = 180;          // seconds: 120, 180, 240 or 300
    int goals = 5;           // 3 to 10
    int best_of = 3;         // games in a series: 1, 3, 5, 7 or 9
    int stadium_cheat = 0;   // eEnvironmentCheat: 0 none, 1 ENV_CHEAT_SECURE ... 5
    int player_cheat = 0;    // ePlayerCheat: 0 none, 1 PLAYER_CHEAT_DEVASTATING ... 4
    int powerup_cheat = 0;   // ePowerupCheat: 0 none, 1 POWERUP_CHEAT_EXPLOSIVES ... 11
};
bool operator==(const MatchSettings& a, const MatchSettings& b);
inline bool operator!=(const MatchSettings& a, const MatchSettings& b) { return !(a == b); }

// Whether every setting is one the game's settings screen offers. Cheats
// count whether or not the save has unlocked them.
bool ValidMatchSettings(const MatchSettings& settings);

struct MatchSaveStatus {
    GameSaveState state = GameSaveState::Missing;
    std::array<MatchSettings, kMatchModeCount> modes{}; // Ready only, by MatchMode
    std::string error;                                  // Unusable only
};

// The match settings in the game save of the NAND backing at `nand_root`, as
// stored (a value the settings screen does not offer stays as it is). Reads only.
MatchSaveStatus InstalledMatchSettings(const std::filesystem::path& nand_root, const SaveTitle& title);

// Write `settings` as the game save's settings for `mode`, as WriteSidekicks
// writes lineups: the checksum follows, the rest of the save stays (the
// per-team switches of GameplaySettings included), the save is
// first copied below `backup_parent` when there is one, and nothing changes
// when the save already holds them. Throws std::invalid_argument for settings
// ValidMatchSettings refuses.
std::filesystem::path WriteMatchSettings(const std::filesystem::path& nand_root, const SaveTitle& title,
                                         MatchMode mode, const MatchSettings& settings,
                                         const std::filesystem::path& backup_parent);
} // namespace mscharged::platform
