// Match presets (the launcher's Matches page): the built-in ones, the file
// format, and the files of the presets folder.
#include "launcher/match_presets.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace mscharged::launcher;
using mscharged::platform::MatchSettings;

namespace
{
namespace fs = std::filesystem;
unsigned checks{};
void Check(bool value, const std::string& message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template <class F> void Reject(F f, const std::string& message)
{
    bool failed{};
    try { f(); } catch (const std::exception&) { failed = true; }
    Check(failed, message);
}
void Put(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

void BuiltIn()
{
    const auto& presets = BuiltInMatchPresets();
    Check(presets.size() == 2 && presets[0].name == "Default" && presets[1].name == "Competitive"
              && presets[0].built_in && presets[1].built_in,
          "Default and Competitive are the built-in presets");
    Check(presets[0].settings == MatchSettings{}, "Default is a new save's settings");
    MatchSettings competitive;
    competitive.skill = 3;
    competitive.best_of = 3;
    competitive.goal_limit = true;
    competitive.goals = 10;
    competitive.stadium_cheat = 1;
    Check(presets[1].settings == competitive, "Competitive: Superstar, best of 3, first to 10 goals, Secure");
    Check(MatchSummary(presets[0].settings) == "Rookie, best of 3, 3 minutes", MatchSummary(presets[0].settings));
    Check(MatchSummary(presets[1].settings) == "Superstar, best of 3, first to 10 goals, Secure Stadiums",
          MatchSummary(presets[1].settings));
    MatchSettings odd;
    odd.skill = 7;
    odd.time = 150;
    odd.player_cheat = 4;
    odd.powerup_cheat = 6;
    odd.stadium_cheat = 9;
    Check(MatchSummary(odd) == "skill 7, best of 3, 150 seconds, cheat 9, Classic Mode, Peelin' Out", MatchSummary(odd));
}

void Names()
{
    for (const char* name : {"Tournament", "Quick 3", "x", "12345678901234567890123456789012", "Àlex's"})
        Check(ValidMatchPresetName(name), std::string("Valid name refused: ") + name);
    for (const char* name : {"", " Quick", "Quick ", "default", "COMPETITIVE", "123456789012345678901234567890123",
                             "a\"b", "a=b", "a[b", "a|b", "a\tb"})
        Check(!ValidMatchPresetName(name), std::string("Invalid name accepted: ") + name);
    Check(SameMatchPresetName("Quick", "qUICK") && !SameMatchPresetName("Quick", "Quick 2"), "Names compare without case");
    Check(MatchPresetFileName("Quick 3") == "quick-3.ini" && MatchPresetFileName("  Best -- of / 5! ") == "best-of-5.ini"
              && MatchPresetFileName("Àà") == "preset.ini",
          "File names");
}

void Text()
{
    MatchPreset preset{"Chaos", {}, false, {}};
    auto& s = preset.settings;
    s.skill = 5;
    s.best_of = 9;
    s.goal_limit = true;
    s.time = 300;
    s.goals = 7;
    s.stadium_cheat = 5;
    s.player_cheat = 3;
    s.powerup_cheat = 11;
    const auto text = MatchPresetText(preset);
    Check(text.find("skill = megastriker\n") != std::string::npos && text.find("stadium_cheat = white_ball\n") != std::string::npos
              && text.find("powerup_cheat = butterfingers\n") != std::string::npos,
          "Settings written by name");
    auto parsed = ParseMatchPreset(text);
    Check(parsed && parsed->name == "Chaos" && parsed->settings == s && !parsed->built_in, "A preset's text read back");
    for (int cheat = 0; cheat < 12; ++cheat)
    {
        s.powerup_cheat = cheat;
        parsed = ParseMatchPreset(MatchPresetText(preset));
        Check(parsed && parsed->settings.powerup_cheat == cheat, "Power-up cheat " + std::to_string(cheat) + " read back");
    }

    // Keys left out are a new save's; unknown keys (an earlier build's
    // home_powerups) and comments are skipped.
    parsed = ParseMatchPreset("; a comment\n# another\r\nname = Mine\r\nGOALS = 9\nwin_by = Goals\nfuture_key = 1\n"
                              "home_powerups = false\n");
    MatchSettings expected;
    expected.goals = 9;
    expected.goal_limit = true;
    Check(parsed && parsed->name == "Mine" && parsed->settings == expected, "A short preset file");

    // Comments after a value; a quoted name keeps its # and ;.
    parsed = ParseMatchPreset("name = \"No. #1; best\"  # the name\nskill = legend   # 4\nbest_of = 5;five\n");
    expected = {};
    expected.skill = 4;
    expected.best_of = 5;
    Check(parsed && parsed->name == "No. #1; best" && parsed->settings == expected, "Comments after values");

    std::string error;
    Check(!ParseMatchPreset("name = \"Open\n", &error) && error == "line 1: no closing quote", error);
    Check(!ParseMatchPreset("goals = 9\n", &error) && error == "no name", "A preset without a name: " + error);
    Check(!ParseMatchPreset("name = Mine\n\ngoals = 11\n", &error) && error == "a setting the game does not offer",
          "Eleven goals: " + error);
    Check(!ParseMatchPreset("name = Mine\nskill = godlike\n", &error) && error == "line 2: skill cannot be godlike",
          "An unknown skill: " + error);
    Check(!ParseMatchPreset("name = Mine\nminutes\n", &error) && error == "line 2: no \"key = value\"", error);
    Check(!ParseMatchPreset("name = \"Default\"\n", &error), "A built-in preset's name in a file");
    Check(!ParseMatchPreset("name = Mine\nbest_of = 4\n"), "A series of 4");
    Check(!ParseMatchPreset("name = Mine\nplayer_cheat = Classic Mode\n"), "A cheat by its label, not its file name");
}

void Folder(const fs::path& root)
{
    fs::remove_all(root);
    const auto folder = root / "match-presets";
    auto list = LoadMatchPresets(folder);
    Check(list.presets.size() == 2 && list.skipped.empty(), "A missing folder lists the built-in presets");

    MatchPreset quick{"Quick 3", {}, false, {}};
    quick.settings.goal_limit = true;
    quick.settings.goals = 3;
    const auto file = SaveMatchPreset(folder, quick, list.presets);
    Check(file == folder / "quick-3.ini" && fs::exists(file) && !fs::exists(folder / "quick-3.ini.new"),
          "A new preset's file");
    MatchPreset alpha{"alpha", {}, false, {}};
    alpha.settings.skill = 2;
    SaveMatchPreset(folder, alpha, LoadMatchPresets(folder).presets);
    list = LoadMatchPresets(folder);
    Check(list.presets.size() == 4 && list.presets[2].name == "alpha" && list.presets[3].name == "Quick 3"
              && list.presets[3].file == file && list.presets[3].settings == quick.settings,
          "Presets listed after the built-in ones, by name");
    Check(FindMatchPreset(list.presets, quick.settings) == &list.presets[3]
              && FindMatchPreset(list.presets, MatchSettings{}) == &list.presets[0],
          "The preset with given settings");

    // The same name, another case, a built-in name, or another preset's file: refused.
    Reject([&] { SaveMatchPreset(folder, MatchPreset{"QUICK 3", {}, false, {}}, list.presets); }, "A name taken twice");
    Reject([&] { SaveMatchPreset(folder, MatchPreset{"Competitive", {}, false, {}}, list.presets); }, "A built-in name");
    Reject([&] { SaveMatchPreset(folder, list.presets[1], list.presets); }, "A built-in preset changed");
    Reject([&] { SaveMatchPreset(folder, MatchPreset{"quick-3", {}, false, {}}, list.presets); },
           "Another preset's file replaced");
    MatchPreset wrong{"Wrong", {}, false, {}};
    wrong.settings.time = 100;
    Reject([&] { SaveMatchPreset(folder, wrong, list.presets); }, "Settings the game does not offer saved");

    // Renamed and changed: the old file goes.
    auto renamed = list.presets[3];
    renamed.name = "Sprint";
    renamed.settings.goals = 4;
    Check(SaveMatchPreset(folder, renamed, list.presets) == folder / "sprint.ini" && !fs::exists(file),
          "A renamed preset's file");
    // Changed under the same name: the same file.
    auto changed = LoadMatchPresets(folder).presets[3];
    changed.settings.best_of = 5;
    SaveMatchPreset(folder, changed, LoadMatchPresets(folder).presets);
    list = LoadMatchPresets(folder);
    Check(list.presets.size() == 4 && list.presets[3].name == "Sprint" && list.presets[3].settings.best_of == 5
              && list.presets[3].settings.goals == 4,
          "A changed preset read back");

    // Files that are not presets are listed apart; so is a second file of a name.
    Put(folder / "broken.ini", "name = Broken\nskill = 9\n");
    Put(folder / "twin.ini", "name = SPRINT\n");
    Put(folder / "notes.txt", "not a preset");
    list = LoadMatchPresets(folder);
    Check(list.presets.size() == 4 && list.skipped.size() == 2, "Files that are not presets skipped");

    fs::remove(folder / "twin.ini");
    list = LoadMatchPresets(folder);
    Check(list.presets[3].name == "Sprint", "Sprint without its twin");
    DeleteMatchPreset(list.presets[3]);
    Check(!fs::exists(folder / "sprint.ini"), "A deleted preset's file");
    Reject([&] { DeleteMatchPreset(list.presets[0]); }, "A built-in preset deleted");
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("usage: launcher_match_presets_tests DATA_DIRECTORY");
        BuiltIn();
        Names();
        Text();
        Folder(fs::path(argv[1]));
        std::cout << "launcher_match_presets: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "launcher_match_presets: " << e.what() << '\n';
        return 1;
    }
}
