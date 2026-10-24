#pragma once

#include "platform/save_import.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Match presets: named sets of match settings and cheats (the Matches page),
// each a small INI file in a folder beside recharged.ini. Choosing one writes
// it into the game save, for matches on this computer or online with friends.
// Two are built in and cannot change: Default and Competitive. Independent of
// SDL/ImGui so it can be unit tested.
namespace mscharged::launcher
{
struct MatchPreset
{
    std::string name;
    platform::MatchSettings settings;
    bool built_in = false;
    std::filesystem::path file; // the preset's file; empty for a built-in one
};

// Default: what a new save starts with, no cheats. Competitive: Superstar,
// best of 3, first to 10 goals, Secure, no other cheat.
const std::vector<MatchPreset>& BuiltInMatchPresets();

// Names are compared without case. A preset's name has 1 to 32 characters,
// no spaces around them, none of " [ ] = | or control characters, and is not
// a built-in preset's.
inline constexpr std::size_t kMaxMatchPresetName = 32;
bool ValidMatchPresetName(const std::string& name);
bool SameMatchPresetName(const std::string& a, const std::string& b);

// The labels the page shows: the game menu's names. A preset file names the
// values its own way (the enums'), so these can change.
const std::vector<const char*>& SkillNames();         // by skill - 1: Rookie ... Megastriker
const std::vector<const char*>& StadiumCheatNames();  // by eEnvironmentCheat: None, Secure Stadiums ...
const std::vector<const char*>& PlayerCheatNames();   // by ePlayerCheat: None, Devastating Hits ...
const std::vector<const char*>& PowerupCheatNames();  // by ePowerupCheat: None, Explosives ...

// One line about the settings: "Superstar, best of 3, first to 10 goals, Secure Stadiums".
std::string MatchSummary(const platform::MatchSettings& settings);

// A preset file's text, and the preset in one: keys left out keep a new
// save's settings, unknown keys are skipped. Empty, with `error` set, for a
// file that is not one.
std::string MatchPresetText(const MatchPreset& preset);
std::optional<MatchPreset> ParseMatchPreset(std::string_view text, std::string* error = nullptr);

// The file name a preset gets: its name in lower case, letters and digits
// kept, "-" for the rest ("Quick 3" quick-3.ini).
std::string MatchPresetFileName(const std::string& name);

struct MatchPresetList
{
    std::vector<MatchPreset> presets; // the built-in ones first, then the folder's by name
    std::vector<std::string> skipped; // files that are not presets, and why
};

// The built-in presets and the .ini files of `folder` (none when it does not exist).
MatchPresetList LoadMatchPresets(const std::filesystem::path& folder);

// Write `preset` as a file in `folder` (created when missing) and return it.
// `preset.file`, when set, is the file it was loaded from, replaced (renamed
// with the preset). Throws std::invalid_argument for a built-in preset, an
// invalid name or settings, or a name another preset of `others` has, and a
// readable std::runtime_error when the file cannot be written; nothing
// changes then.
std::filesystem::path SaveMatchPreset(const std::filesystem::path& folder, const MatchPreset& preset,
                                      const std::vector<MatchPreset>& others);

// Remove a preset's file. Throws for a built-in preset or a file that cannot be removed.
void DeleteMatchPreset(const MatchPreset& preset);

// The first preset with these settings (a built-in one before the others).
const MatchPreset* FindMatchPreset(const std::vector<MatchPreset>& presets, const platform::MatchSettings& settings);
} // namespace mscharged::launcher
