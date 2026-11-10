#include "ui/rui_pages.h"

#include "bootstrap/advanced_choices.h"
#include "bootstrap/game_tweaks.h"
#include "bootstrap/sound_groups.h"
#include "bootstrap/texts.h"
#include "overlay/flags.h"
#include "platform/competitive_rules.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace mscharged::ui
{
namespace
{
using texts::T;

std::string Percent(int value) { return std::to_string(value) + " %"; }

// A switch of a setting: its new state as the change.
void SettingSwitch(List& list, PageActions& actions, const char* label, bool on, overlay::GameSetting setting,
                   const char* about = nullptr)
{
    if (const auto next = list.Switch(label, on, about))
        actions.setting = overlay::GameSettingChange{setting, *next ? 1 : 0};
}

// A volume in percent, in steps of 5 over 20 bars.
std::optional<int> VolumeBars(List& list, const char* label, int value, const char* about)
{
    const std::string text = Percent(value);
    return list.Bars(label, value, 0, 100, 5, 20, text.c_str(), about);
}

void DrawMenu(List& list, PageActions& actions)
{
    if (list.Plate("ONLINE"))
        actions.open = PageId::Online;
    if (list.Plate("OPTIONS")) actions.open = PageId::Options;
    if (list.Plate("QUIT RECHARGED")) actions.open = PageId::Quit;
}

void DrawQuit(const PageModel& model, List& list, PageActions& actions)
{
    list.Note(model.front_end ? "Quit Recharged? The game closes."
                              : "Quit Recharged? The game closes, and the match is not finished.");
    if (list.Plate("QUIT RECHARGED")) actions.quit = true;
    if (list.Plate("CANCEL")) actions.back = true;
}

void DrawOptions(const PageModel& model, List& list, PageActions& actions)
{
    if (list.Plate("GAMEPLAY"))
        actions.open = PageId::Gameplay;
    if (list.Plate("SETTINGS")) actions.open = PageId::Settings;
    if (model.front_end && list.Plate("CREDITS")) actions.credits = true;
}

void DrawGameplay(int category, const PageModel& model, List& list, PageActions& actions)
{
    const auto& groups = overlay::GameOptionGroups();
    if (groups.empty()) return;
    const auto& group = groups[std::size_t(std::clamp(category, 0, int(groups.size()) - 1))];
    list.Note(group.about);
    if (model.overlay.options.in_match) list.Note("A match is on: it keeps the options it started with.");
    for (const GameTweak tweak : group.tweaks)
    {
        const std::size_t index = GameTweakIndex(tweak);
        const GameTweakInfo& info = kGameTweaks[index];
        const bool on = model.overlay.options.tweaks[index];
        if (const auto next = list.Switch(info.name, on, info.description))
            actions.option = overlay::GameOptionChange{tweak, *next};
    }
}

void DrawDisplay(const PageModel& model, List& list, PageActions& actions)
{
    using overlay::GameSetting;
    using overlay::GameSettingChange;
    const auto& settings = model.overlay.settings;
    if (model.game.known && model.network_match)
        list.Note("The match camera can be changed outside online matches.");
    else if (model.game.known)
    {
        list.Caption("MATCH CAMERA");
        // The game's Visual Options, in its words.
        if (const auto on = list.Switch("Camera", model.game.auto_zoom,
                                        "DYNAMIC zooms in and out with the play; STATIC keeps the zoom level below. "
                                        "Saved with the game, as in its Visual Options.",
                                        "STATIC", "DYNAMIC"))
            actions.auto_zoom = *on;
        static const std::vector<std::string> kLevels{"1", "2", "3", "4", "5"};
        if (const auto zoom = list.Choice("Zoom level", kLevels, model.game.zoom,
                                          "How far the camera stays from the play (1 nearest), when it does not "
                                          "zoom by itself."))
            actions.zoom = *zoom;
    }
    list.Caption("WINDOW");
    SettingSwitch(list, actions, "Fullscreen", settings.fullscreen, GameSetting::Fullscreen);
    if (settings.pictures.size() > 1)
        if (const auto picked = list.Choice("Picture", settings.pictures, settings.picture,
                                            settings.picture_about.c_str()))
            actions.setting = GameSettingChange{GameSetting::Picture, *picked};
    list.Caption("RENDERING");
    if (!settings.resolutions.empty())
        if (const auto picked = list.Choice("3D resolution", settings.resolutions, settings.resolution,
                                            settings.resolution_about.c_str()))
            actions.setting = GameSettingChange{GameSetting::Resolution, *picked};
    SettingSwitch(list, actions, "Performance numbers", settings.performance_numbers, GameSetting::PerformanceNumbers,
                  "The frame rate and how busy the graphics card and processor are, in a corner of the picture.");
}

void DrawAdvanced(const PageModel& model, List& list, PageActions& actions)
{
    using overlay::GameSetting;
    const auto& settings = model.overlay.settings;
    // A choice of a list, stepped through by name.
    const auto cycle = [&](const char* label, const std::vector<std::string>& names, int chosen, const char* about,
                           GameSetting setting) {
        if (names.empty()) return;
        const int count = int(names.size());
        chosen = std::clamp(chosen, 0, count - 1);
        if (count == 1)
        {
            list.Row(label, names.front().c_str(), about);
            return;
        }
        if (const auto step = list.Cycle(label, names[std::size_t(chosen)].c_str(), about))
            actions.setting = overlay::GameSettingChange{setting, (chosen + *step + count) % count};
    };
    list.Caption("LAUNCHER");
    if (settings.reopen_known)
        SettingSwitch(list, actions, "Reopen the launcher", settings.reopen_launcher, GameSetting::ReopenLauncher,
                      "The launcher opens again when the game closes.");
    else list.Note("The game started without the launcher: closing it opens nothing.");
    cycle("Launcher size", settings.launcher_sizes, settings.launcher_size,
          "Scales the launcher. Automatic follows your display's scaling setting.", GameSetting::LauncherSize);

    list.Caption("GRAPHICS (NEXT START)");
    cycle("Graphics API", settings.graphics_apis, settings.graphics_api,
          settings.graphics_apis.size() > 1 ? "Automatic uses Vulkan, or DirectX 12 when Vulkan cannot start. Used "
                                              "the next time you start the game."
                                            : "The one this computer's game uses.",
          GameSetting::GraphicsApi);
    if (settings.graphics_cards_shown)
    {
        if (settings.graphics_cards_listing && settings.graphics_cards.size() <= 1)
            list.Row("Graphics card", "Looking...", "Looking for the computer's graphics cards.");
        else
            cycle("Graphics card", settings.graphics_cards, settings.graphics_card,
                  "Automatic uses the fastest. Used the next time you start the game.", GameSetting::GraphicsCard);
    }

    list.Caption("DIAGNOSTICS");
    SettingSwitch(list, actions, "Performance stats", settings.frame_logging, GameSetting::FrameLogging,
                  "Records frame times for each game session, with the arena and settings, in the logs folder. From "
                  "the next start.");
    SettingSwitch(list, actions, "API validation", settings.graphics_validation, GameSetting::GraphicsValidation,
                  "Checks every graphics call for errors. Useful for bug reports, but slower. From the next start.");
    static const std::vector<std::string> kDetails = [] {
        std::vector<std::string> names;
        for (const auto& level : kLogLevels) names.push_back(level.label);
        return names;
    }();
    if (const auto picked = list.Choice("Log detail", kDetails, settings.log_detail,
                                        "Which messages reach the console and the log. From the next start."))
        actions.setting = overlay::GameSettingChange{GameSetting::LogDetail, *picked};
    SettingSwitch(list, actions, "Verbose console", settings.verbose_console, GameSetting::VerboseConsole,
                  "Also prints development traces and the game's own debug output.");
    if (settings.log_window_shown)
        SettingSwitch(list, actions, "Log window", settings.log_window, GameSetting::LogWindow,
                      "Shows the log in a separate window. Closing it doesn't stop the game.");
    list.Note("The settings file, the folders and resetting everything are in the launcher's Advanced page.");
}

void DrawSound(const PageModel& model, List& list, PageActions& actions)
{
    using overlay::GameSetting;
    using overlay::GameSettingChange;
    const auto& settings = model.overlay.settings;
    list.Caption("MASTER");
    if (const auto volume = VolumeBars(list, "Master volume", settings.master_volume, "Everything the game plays."))
        actions.setting = GameSettingChange{GameSetting::MasterVolume, *volume};
    SettingSwitch(list, actions, "Mute", settings.mute, GameSetting::Mute);
    list.Caption("SOUND LEVELS");
    for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
        if (const auto volume =
                VolumeBars(list, kSoundGroups[n].name, settings.sound_volumes[n], kSoundGroups[n].description))
            actions.setting = GameSettingChange{GameSetting::SoundVolume, *volume, int(n)};
    if (model.game.known && !model.network_match)
    {
        list.Caption("THE GAME'S OWN LEVELS");
        constexpr const char* kAbout =
            "The game's own volume bars, saved with the game as in its Audio Options; Recharged's levels above "
            "work on top of them.";
        // Each bar 0-10, its level as the game shows it.
        const auto bar = [&](const char* label, int value, std::optional<int>& out) {
            const std::string text = std::to_string(value);
            if (const auto next = list.Bars(label, value, 0, 10, 1, 10, text.c_str(), kAbout)) out = *next;
        };
        bar("Game music", model.game.music, actions.music);
        bar("Game sound effects", model.game.effects, actions.effects);
        bar("Game voices", model.game.voices, actions.voices);
    }
    list.Caption("ONLINE");
    SettingSwitch(list, actions, "Challenge sound", settings.challenge_sound, GameSetting::ChallengeSound,
                  "A chime when someone challenges you or accepts your challenge.");
}

// The online pages ---------------------------------------------------------

Tone ToneOf(overlay::Tone tone)
{
    switch (tone)
    {
    case overlay::Tone::Good: return Tone::Good;
    case overlay::Tone::Busy: return Tone::Busy;
    case overlay::Tone::Away: return Tone::Away;
    case overlay::Tone::Bad: return Tone::Bad;
    default: return Tone::Neutral;
    }
}

Person PersonOf(const PageModel& model, const std::string& image, const std::string& initials, std::int64_t tint,
                int presence, const std::string& country)
{
    Person person;
    if (model.image && !image.empty()) person.image = (*model.image)(image);
    person.initials = initials;
    person.tint = tint;
    person.presence = presence;
    if (model.image && !country.empty()) person.flags = (*model.image)(std::string(overlay::kFlagAtlas));
    person.country = country;
    return person;
}

Person PersonOf(const PageModel& model, const overlay::OnlinePanelRow& row, bool presence = true)
{
    const int dot = !presence ? -1 : row.presence == overlay::Presence::Free ? 0
                    : row.presence == overlay::Presence::Busy ? 1 : 2;
    return PersonOf(model, row.image, row.initials, row.tint_seed, dot, row.country);
}

// A player's setup (or the search's) starts from the preset used last.
void OpenSetup(const PageModel& model, PageState& state, PageActions& actions, const std::string& key)
{
    overlay::StartOnlinePanelSetup(state.online, model.overlay.online, key);
    actions.open = PageId::Setup;
    actions.open_key = key;
}

void DrawOnline(const PageModel& model, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    if (list.Plate("PLAYERS")) actions.open = PageId::Players;
    if (list.Plate("RANKED")) actions.open = PageId::Ranked;
    if (list.Plate("LEADERBOARD")) actions.open = PageId::Leaderboard;
    if (model.from_wheel && list.Plate("WII ONLINE"))
        actions.wii_online = true;
    // A set waiting for the player (its page is PLAYERS).
    if (overlay::OnlinePanelWaiting(online) > 0)
    {
        const std::string waits = "Your set waits for you: " + online.set.title + ".";
        list.Note(waits.c_str());
    }
}

void DrawYou(const PageModel& model, PageState& state, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    if (online.signed_out)
    {
        list.Card(nullptr, "NO PLAYER PROFILE", "Online needs a player profile",
                  {"Make one on the launcher's Player profile page: then friends, challenges, sets and the "
                   "leaderboards are here."});
        return;
    }
    const auto& me = online.me;
    const Person you = PersonOf(model, me.image, me.initials, me.tint_seed, -1, me.country);
    std::vector<std::string> lines;
    if (!me.detail.empty()) lines.push_back(me.detail);
    lines.push_back(me.available ? "Open to challenges" : "Not open to challenges");
    list.Card(&you, "YOU", me.name.c_str(), lines, me.available ? Tone::Good : Tone::Away);
    if (!online.message.empty()) list.Note(online.message.c_str());
    // The set: what it is, and what the player can do now.
    const auto& set = online.set;
    if (set.stage != overlay::OnlinePanelSet::Stage::None)
    {
        const Person opponent = PersonOf(model, set.opponent);
        std::vector<std::string> about{set.status};
        if (!set.rules.empty()) about.push_back(set.rules);
        list.Card(&opponent, "YOUR SET", set.title.c_str(), about, ToneOf(set.tone), set.games, set.won, set.lost);
        if (set.can_accept && list.Plate("ACCEPT")) actions.online.accept = true;
        if (set.can_decline && list.Plate("DECLINE")) actions.online.decline = true;
        if (set.can_ready && list.Plate("READY TO PLAY!")) actions.online.ready = true;
        if (set.can_rematch && list.Plate("REMATCH")) actions.online.rematch = true;
        if (set.can_cancel)
        {
            const std::string label = Upper(set.cancel_label.empty() ? "CANCEL SET" : set.cancel_label.c_str());
            if (list.Plate(label.c_str())) actions.online.cancel = true;
        }
    }
    list.Caption("CHALLENGES");
    if (me.can_set_available)
        if (const auto on = list.Switch("Open to challenges", me.available,
                                        "Others see you as available and can challenge you, also to random games."))
            actions.online.set_available = *on;
    // Random opponents: the search, or its setup.
    const auto& search = online.search;
    if (search.active)
    {
        const std::string text = "Searching by " + search.rules + ": " + std::to_string(search.seconds) + " s, "
            + std::to_string(search.asked) + " asked.";
        list.Note(text.c_str());
        if (list.Plate("STOP SEARCHING")) actions.online.stop_search = true;
    }
    else if (online.can_search)
    {
        const char* rules = online.presets.empty()
            ? "" : online.presets[std::size_t(overlay::ChosenOnlinePanelPreset(online))].name.c_str();
        if (list.Row("Find an opponent", rules, "A random opponent, by the rules you choose."))
            OpenSetup(model, state, actions, kSearchKey);
    }
    if (!online.note.empty()) list.Note(online.note.c_str());
}

void DrawSection(const PageModel& model, List& list, PageActions& actions, overlay::OnlinePanelSection::Id id)
{
    const auto& online = model.overlay.online;
    if (online.signed_out)
    {
        list.Note("Online needs a player profile: the launcher's Player profile page makes one.");
        return;
    }
    for (const auto& section : online.sections)
    {
        if (section.id != id) continue;
        if (!section.aside.empty()) list.Note(section.aside.c_str());
        if (section.rows.empty())
        {
            list.Note(section.empty.c_str());
            if (!section.hint.empty()) list.Note(section.hint.c_str());
        }
        for (const auto& row : section.rows)
        {
            const Person person = PersonOf(model, row);
            const char* about = row.detail.empty() ? nullptr : row.detail.c_str();
            if (list.PlayerRow(person, row.name.c_str(), row.status.c_str(), ToneOf(row.tone),
                               row.version.empty() ? nullptr : row.version.c_str(), about))
            {
                actions.open = PageId::Player;
                actions.open_key = row.key;
            }
        }
    }
}

void DrawPlayer(const std::string& key, const PageModel& model, PageState& state, List& list, PageActions& actions)
{
    const auto* row = overlay::FindRow(model.overlay.online, key);
    if (!row)
    {
        list.Note("Not online any more.");
        return;
    }
    const Person person = PersonOf(model, *row);
    std::vector<std::string> lines{row->status};
    if (!row->detail.empty()) lines.push_back(row->detail);
    if (!row->version.empty()) lines.push_back("Plays the " + row->version + " version");
    list.Card(&person, row->is_friend ? "FRIEND" : "PLAYER", row->name.c_str(), lines, ToneOf(row->tone));
    if (row->can_challenge)
    {
        if (list.Plate("CHALLENGE", "A set by the rules you choose.")) OpenSetup(model, state, actions, row->key);
    }
    else if (!row->blocked.empty()) list.Note(row->blocked.c_str());
    if (row->can_befriend)
    {
        const bool mii = row->kind == overlay::OnlinePanelRow::Kind::Mii;
        if (row->is_friend)
        {
            if (list.Plate("REMOVE FRIEND"))
                mii ? void(actions.online.remove_mii_friend = row->mii_profile)
                    : void(actions.online.remove_friend = row->player_id);
        }
        else if (list.Plate("ADD FRIEND"))
            mii ? void(actions.online.add_mii_friend = row->mii_profile) : void(actions.online.add_friend = row->player_id);
    }
}

std::string GameLength(int seconds)
{
    if (seconds % 60 == 0) return std::to_string(seconds / 60) + " min";
    return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}

// One of a few values as buttons (the value kept among them).
void Values(List& list, const char* label, std::vector<int> values, int& value,
            const std::function<std::string(int)>& text)
{
    if (std::find(values.begin(), values.end(), value) == values.end())
    {
        values.push_back(value);
        std::sort(values.begin(), values.end());
    }
    std::vector<std::string> names;
    int chosen = 0;
    for (std::size_t n = 0; n < values.size(); ++n)
    {
        names.push_back(text(values[n]));
        if (values[n] == value) chosen = int(n);
    }
    if (const auto picked = list.Choice(label, names, chosen)) value = values[std::size_t(*picked)];
}

// One of the choices of a list, stepped through by name.
void Choices(List& list, const char* label, const std::vector<platform::RuleChoice>& choices, std::string& key)
{
    if (const auto step = list.Cycle(label, platform::RuleChoiceLabel(choices, key).c_str()))
        key = platform::StepRuleChoice(choices, key, *step);
}

void DrawSetup(const std::string& key, const PageModel& model, PageState& state, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    const bool search = key == kSearchKey;
    const auto* row = search ? nullptr : overlay::FindRow(online, key);
    auto& setup = state.online;
    if ((!search && !row) || online.presets.empty() || setup.setup_for != key)
    {
        list.Note(!search && !row ? "Not online any more." : "This server has no rules to play sets by yet.");
        return;
    }
    const int count = int(online.presets.size());
    setup.preset = std::clamp(setup.preset, 0, count - 1);
    const auto& preset = online.presets[std::size_t(setup.preset)];
    auto& rules = setup.rules;
    const bool changed = !(rules == preset.rules);
    // A search runs from its request until the server has it, or says why not.
    const double now = ImGui::GetTime();
    const bool searching = overlay::OnlinePanelSearchRunning(online, setup, now) && search;
    if (row)
    {
        const Person person = PersonOf(model, *row);
        list.Card(&person, "CHALLENGE", row->name.c_str(), {row->status}, ToneOf(row->tone));
    }
    else
    {
        const std::string status = searching ? "Searching: " + std::to_string(online.search.seconds) + " s, "
            + std::to_string(online.search.asked) + " asked" : std::string("Anybody open to random games");
        list.Card(nullptr, "RANDOM OPPONENT", "Find an opponent", {status}, searching ? Tone::Busy : Tone::Neutral);
    }
    // The way on first, the rules under it.
    const bool can = search ? online.can_search || searching : row->can_challenge;
    const char* label = !search ? "SEND CHALLENGE" : searching ? "STOP SEARCHING" : "FIND AN OPPONENT NOW!";
    if (list.Plate(label, changed ? "Changed for this one only: the preset stays as it is." : nullptr, can))
    {
        if (searching)
        {
            actions.online.stop_search = true;
            setup.search_sent = -100.0;
        }
        else
        {
            const auto challenge = overlay::MakeOnlinePanelChallenge(preset, rules, search ? 0 : row->player_id);
            actions.online.set_challenge_rules = preset.name;
            if (search)
            {
                actions.online.search = challenge;
                setup.search_sent = now;
            }
            else
            {
                actions.online.challenge = challenge;
                actions.back = true;
            }
        }
    }
    if (searching) return;
    list.Caption("PRESET");
    if (const auto step = list.Cycle("Start from", preset.name.c_str()))
    {
        setup.preset = (setup.preset + *step + count) % count;
        rules = online.presets[std::size_t(setup.preset)].rules;
    }
    list.Caption("MATCH");
    Values(list, "Series", {1, 3, 5, 7}, rules.games, [](int n) { return std::to_string(n); });
    static const std::vector<std::string> kLimits{"Time", "Goals"};
    if (const auto picked = list.Choice("Win by", kLimits, rules.limit == "goals" ? 1 : 0))
        rules.limit = *picked == 1 ? "goals" : "time";
    if (rules.limit == "goals")
        Values(list, "Goals", {3, 4, 5, 6, 7, 8, 9, 10}, rules.goals, [](int n) { return std::to_string(n); });
    else Values(list, "Time", {120, 180, 240, 300}, rules.time, GameLength);
    Choices(list, "Stadium", platform::StadiumChoices(), rules.stadium);
    list.Caption("CHEATS");
    Choices(list, "Stadium cheat", platform::StadiumCheatChoices(), rules.stadium_cheat);
    Choices(list, "Player cheat", platform::PlayerCheatChoices(), rules.player_cheat);
    Choices(list, "Power-up cheat", platform::PowerupCheatChoices(), rules.powerup_cheat);
    list.Caption("GAMEPLAY");
    for (const auto& tweak : platform::RuleTweakChoices())
    {
        const auto found = std::find(rules.tweaks.begin(), rules.tweaks.end(), tweak.key);
        const bool on = found != rules.tweaks.end();
        if (const auto next = list.Switch(tweak.label, on))
        {
            if (!*next) rules.tweaks.erase(found);
            else
            {
                rules.tweaks.push_back(tweak.key);
                std::sort(rules.tweaks.begin(), rules.tweaks.end());
            }
        }
    }
}

void DrawRanked(const PageModel& model, List& list, PageActions& actions)
{
    const auto& ranked = model.overlay.ranked;
    static const std::vector<std::string> kLines{"Play anybody online, as from the game's online menu.",
                                                 "Your wins and losses go on the leaderboards."};
    list.Card(nullptr, "RANKED", "Play RANKED", kLines);
    if (ranked.going)
    {
        list.Note(ranked.waiting ? "On the way to RANKED: go back to the main menu or the online menu, the game goes on "
                                   "from there."
                                 : "On the way to RANKED...");
        if (list.Plate("STOP")) actions.ranked.stop = true;
    }
    else
    {
        if (list.Plate("PLAY RANKED", ranked.own_mii ? "With your own Mii." : "You choose your Mii in the game.",
                       ranked.can_play))
            actions.ranked.play = true;
        if (!ranked.blocked.empty()) list.Note(ranked.blocked.c_str());
    }
    if (list.Row("Leaderboard", "", "The season's and today's leaders.")) actions.open = PageId::Leaderboard;
}

std::string Record(const platform::LeaderboardEntry& entry)
{
    return (entry.wins ? std::to_string(*entry.wins) : std::string("-")) + " \xE2\x80\x93 "
        + (entry.losses ? std::to_string(*entry.losses) : std::string("-"));
}

void DrawLeaderboard(int category, const PageModel& model, List& list)
{
    const auto& ranked = model.overlay.ranked;
    if (ranked.signed_out)
    {
        list.Note("The leaderboards need a player profile: the launcher's Player profile page makes one.");
        return;
    }
    if (ranked.missing)
    {
        list.Note("This server has no leaderboards yet.");
        return;
    }
    if (!ranked.boards)
    {
        list.Note("Loading...");
        return;
    }
    const bool daily = category == 1;
    const auto& board = daily ? ranked.boards->daily : ranked.boards->season;
    const std::string days = board.first_day == board.last_day || board.last_day.empty()
        ? board.first_day : board.first_day + "  \xE2\x80\x93  " + board.last_day;
    std::vector<std::string> lines{days};
    if (board.you)
        lines.push_back("You: " + (board.you->rank > 0 ? "place " + std::to_string(board.you->rank) : std::string("-"))
                        + "  \xC2\xB7  " + Record(*board.you) + "  \xC2\xB7  " + std::to_string(board.you->points)
                        + " points");
    list.Card(nullptr, daily ? "TODAY" : "THIS SEASON", (std::to_string(board.players) + " PLAYERS").c_str(), lines);
    if (board.top.empty()) list.Note(daily ? "Nobody has played RANKED today yet." : "Nobody has played RANKED yet.");
    bool youShown = false;
    for (const auto& entry : board.top)
    {
        const std::string name = entry.player.empty() ? entry.mii : entry.player;
        list.BoardRow(entry.rank, name.c_str(), Record(entry).c_str(), std::to_string(entry.points).c_str(), entry.you);
        youShown = youShown || entry.you;
    }
    if (board.you && !youShown)
    {
        list.Caption("YOU");
        const auto& you = *board.you;
        const std::string name = you.player.empty() ? you.mii : you.player;
        list.BoardRow(you.rank, name.c_str(), Record(you).c_str(), std::to_string(you.points).c_str(), true);
    }
}

void DrawMatch(List& list, PageActions& actions)
{
    if (list.Plate("CONTINUE")) actions.close = true;
    if (list.Plate("FORFEIT")) actions.open = PageId::Forfeit;
}

void DrawForfeit(const PageModel& model, List& list, PageActions& actions)
{
    if (model.forfeiting)
    {
        list.Note(T("menus.forfeit.leaving"));
        return;
    }
    list.Note(T(model.set_match ? "menus.forfeit.set_note" : "menus.forfeit.match_note"));
    if (list.Plate("FORFEIT")) actions.forfeit = true;
    if (list.Plate("CANCEL")) actions.back = true;
}

void DrawPrompt(const PageModel& model, List& list, PageActions& actions)
{
    if (!model.prompt) return;
    const auto& prompt = *model.prompt;
    const Person person = PersonOf(model, prompt.opponent, false);
    std::vector<std::string> lines;
    if (!prompt.detail.empty()) lines.push_back(prompt.detail);
    // Its kind ("CHALLENGE") is the page's title.
    list.Card(&person, nullptr, prompt.title.c_str(), lines, Tone::Neutral,
              prompt.has_score ? prompt.games : 0, prompt.won, prompt.lost);
    const std::string primary = Upper(prompt.primary.c_str()), secondary = Upper(prompt.secondary.c_str());
    if (list.Plate(primary.c_str())) actions.answer = 1;
    if (!secondary.empty() && list.Plate(secondary.c_str())) actions.answer = 2;
}
} // namespace

std::string PageTitle(PageId id, const std::string& key, const PageModel& model)
{
    switch (id)
    {
    case PageId::Menu: return "RECHARGED";
    case PageId::Options: return "OPTIONS";
    case PageId::Gameplay: return "GAMEPLAY";
    case PageId::Settings: return "SETTINGS";
    case PageId::Quit: return "QUIT";
    case PageId::Online: return "ONLINE";
    case PageId::Players: return "PLAYERS";
    case PageId::Player:
        if (const auto* row = overlay::FindRow(model.overlay.online, key)) return row->name;
        return "PLAYER";
    case PageId::Setup: return key == kSearchKey ? "RANDOM OPPONENT" : "CHALLENGE";
    case PageId::Ranked: return "RANKED";
    case PageId::Leaderboard: return "LEADERBOARD";
    case PageId::Prompt:
        if (model.prompt && !model.prompt->kicker.empty()) return model.prompt->kicker;
        return "CHALLENGE";
    case PageId::Match: return "MATCH";
    case PageId::Forfeit: return "FORFEIT";
    }
    return "";
}

const std::vector<std::string>& PageCategories(PageId id)
{
    static const std::vector<std::string> kGameplay = [] {
        std::vector<std::string> names;
        for (const auto& group : overlay::GameOptionGroups()) names.push_back(Upper(group.short_title));
        return names;
    }();
    static const std::vector<std::string> kSettings{"DISPLAY", "SOUND", "ADVANCED"};
    static const std::vector<std::string> kPlayers{"YOU", "FRIENDS", "PLAYERS"};
    static const std::vector<std::string> kLeaderboard{"SEASON", "TODAY"};
    static const std::vector<std::string> kNone;
    switch (id)
    {
    case PageId::Gameplay: return kGameplay;
    case PageId::Settings: return kSettings;
    case PageId::Players: return kPlayers;
    case PageId::Leaderboard: return kLeaderboard;
    default: return kNone;
    }
}

bool PageWide(PageId id) { return !PageCategories(id).empty() || id == PageId::Setup; }

bool PageCompact(PageId id) { return id == PageId::Prompt || id == PageId::Match || id == PageId::Forfeit; }

void DrawPage(PageId id, int category, const std::string& key, const PageModel& model, PageState& state, List& list,
              PageActions& actions)
{
    switch (id)
    {
    case PageId::Menu: DrawMenu(list, actions); break;
    case PageId::Options: DrawOptions(model, list, actions); break;
    case PageId::Quit: DrawQuit(model, list, actions); break;
    case PageId::Gameplay: DrawGameplay(category, model, list, actions); break;
    case PageId::Settings:
        if (category == kSettingsSound) DrawSound(model, list, actions);
        else if (category == kSettingsAdvanced) DrawAdvanced(model, list, actions);
        else DrawDisplay(model, list, actions);
        break;
    case PageId::Online: DrawOnline(model, list, actions); break;
    case PageId::Players:
        if (category == 1) DrawSection(model, list, actions, overlay::OnlinePanelSection::Id::Online);
        else if (category == 2) DrawSection(model, list, actions, overlay::OnlinePanelSection::Id::Available);
        else DrawYou(model, state, list, actions);
        break;
    case PageId::Player: DrawPlayer(key, model, state, list, actions); break;
    case PageId::Setup: DrawSetup(key, model, state, list, actions); break;
    case PageId::Ranked: DrawRanked(model, list, actions); break;
    case PageId::Leaderboard: DrawLeaderboard(category, model, list); break;
    case PageId::Prompt: DrawPrompt(model, list, actions); break;
    case PageId::Match: DrawMatch(list, actions); break;
    case PageId::Forfeit: DrawForfeit(model, list, actions); break;
    }
}
} // namespace mscharged::ui
