#include "ui/rui_pages.h"

#include "bootstrap/advanced_choices.h"
#include "bootstrap/game_tweaks.h"
#include "bootstrap/sound_groups.h"
#include "bootstrap/table_texts.h"
#include "bootstrap/texts.h"
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

std::string Percent(int value) { return T("menus.sound.percent", {{"value", std::to_string(value)}}); }

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
    if (list.Plate(T("menus.menu.online")))
        actions.open = PageId::Online;
    if (list.Plate(T("menus.menu.options"))) actions.open = PageId::Options;
    if (list.Plate(T("menus.menu.quit"))) actions.open = PageId::Quit;
}

void DrawQuit(const PageModel& model, List& list, PageActions& actions)
{
    list.Note(model.front_end ? T("menus.quit.note") : T("menus.quit.note_match"));
    if (list.Plate(T("menus.quit.quit"))) actions.quit = true;
    if (list.Plate(T("menus.common.cancel"))) actions.back = true;
}

void DrawOptions(const PageModel& model, List& list, PageActions& actions)
{
    if (list.Plate(T("menus.options.gameplay")))
        actions.open = PageId::Gameplay;
    if (list.Plate(T("menus.options.settings"))) actions.open = PageId::Settings;
    if (model.front_end && list.Plate(T("menus.options.credits"))) actions.credits = true;
}

void DrawGameplay(int category, const PageModel& model, List& list, PageActions& actions)
{
    const auto& groups = overlay::GameOptionGroups();
    if (groups.empty()) return;
    const auto& group = groups[std::size_t(std::clamp(category, 0, int(groups.size()) - 1))];
    list.Note(group.about);
    if (model.overlay.options.in_match) list.Note(T("menus.gameplay.in_match"));
    for (const GameTweak tweak : group.tweaks)
    {
        const std::size_t index = GameTweakIndex(tweak);
        const GameTweakInfo& info = kGameTweaks[index];
        const bool on = model.overlay.options.tweaks[index];
        if (const auto next = list.Switch(texts::TweakName(info), on, texts::TweakAbout(info)))
            actions.option = overlay::GameOptionChange{tweak, *next};
    }
}

void DrawDisplay(const PageModel& model, List& list, PageActions& actions)
{
    using overlay::GameSetting;
    using overlay::GameSettingChange;
    const auto& settings = model.overlay.settings;
    if (model.game.known && model.network_match)
        list.Note(T("menus.display.camera_online"));
    else if (model.game.known)
    {
        list.Caption(T("menus.display.match_camera"));
        // The game's Visual Options, in its words.
        if (const auto on = list.Switch(T("menus.display.camera"), model.game.auto_zoom,
                                        T("menus.display.camera_about"), T("menus.display.static"),
                                        T("menus.display.dynamic")))
            actions.auto_zoom = *on;
        static const std::vector<std::string> kLevels{"1", "2", "3", "4", "5"};
        if (const auto zoom = list.Choice(T("menus.display.zoom_level"), kLevels, model.game.zoom,
                                          T("menus.display.zoom_level_about")))
            actions.zoom = *zoom;
    }
    list.Caption(T("menus.display.window"));
    SettingSwitch(list, actions, T("menus.display.fullscreen"), settings.fullscreen, GameSetting::Fullscreen);
    if (settings.pictures.size() > 1)
        if (const auto picked = list.Choice(T("menus.display.picture"), settings.pictures, settings.picture,
                                            settings.picture_about.c_str()))
            actions.setting = GameSettingChange{GameSetting::Picture, *picked};
    list.Caption(T("menus.display.rendering"));
    if (!settings.resolutions.empty())
        if (const auto picked = list.Choice(T("menus.display.resolution"), settings.resolutions, settings.resolution,
                                            settings.resolution_about.c_str()))
            actions.setting = GameSettingChange{GameSetting::Resolution, *picked};
    SettingSwitch(list, actions, T("menus.display.performance_numbers"), settings.performance_numbers,
                  GameSetting::PerformanceNumbers, T("menus.display.performance_numbers_about"));
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
    list.Caption(T("menus.advanced.launcher"));
    if (settings.reopen_known)
        SettingSwitch(list, actions, T("menus.advanced.reopen_launcher"), settings.reopen_launcher,
                      GameSetting::ReopenLauncher, T("menus.advanced.reopen_launcher_about"));
    else list.Note(T("menus.advanced.no_launcher"));
    cycle(T("menus.advanced.launcher_size"), settings.launcher_sizes, settings.launcher_size,
          T("menus.advanced.launcher_size_about"), GameSetting::LauncherSize);

    list.Caption(T("menus.advanced.graphics"));
    cycle(T("menus.advanced.graphics_api"), settings.graphics_apis, settings.graphics_api,
          settings.graphics_apis.size() > 1 ? T("menus.advanced.graphics_api_about")
                                            : T("menus.advanced.graphics_api_only"),
          GameSetting::GraphicsApi);
    if (settings.graphics_cards_shown)
    {
        if (settings.graphics_cards_listing && settings.graphics_cards.size() <= 1)
            list.Row(T("menus.advanced.graphics_card"), T("menus.advanced.looking"),
                     T("menus.advanced.looking_about"));
        else
            cycle(T("menus.advanced.graphics_card"), settings.graphics_cards, settings.graphics_card,
                  T("menus.advanced.graphics_card_about"), GameSetting::GraphicsCard);
    }

    list.Caption(T("menus.advanced.diagnostics"));
    SettingSwitch(list, actions, T("menus.advanced.performance_stats"), settings.frame_logging,
                  GameSetting::FrameLogging, T("menus.advanced.performance_stats_about"));
    SettingSwitch(list, actions, T("menus.advanced.api_validation"), settings.graphics_validation,
                  GameSetting::GraphicsValidation, T("menus.advanced.api_validation_about"));
    std::vector<std::string> details;
    for (const auto& level : kLogLevels) details.push_back(texts::ChoiceLabel("log_levels", level));
    if (const auto picked = list.Choice(T("menus.advanced.log_detail"), details, settings.log_detail,
                                        T("menus.advanced.log_detail_about")))
        actions.setting = overlay::GameSettingChange{GameSetting::LogDetail, *picked};
    SettingSwitch(list, actions, T("menus.advanced.verbose_console"), settings.verbose_console,
                  GameSetting::VerboseConsole, T("menus.advanced.verbose_console_about"));
    if (settings.log_window_shown)
        SettingSwitch(list, actions, T("menus.advanced.log_window"), settings.log_window, GameSetting::LogWindow,
                      T("menus.advanced.log_window_about"));
    list.Note(T("menus.advanced.launcher_note"));
}

void DrawSound(const PageModel& model, List& list, PageActions& actions)
{
    using overlay::GameSetting;
    using overlay::GameSettingChange;
    const auto& settings = model.overlay.settings;
    list.Caption(T("menus.sound.master"));
    if (const auto volume = VolumeBars(list, T("menus.sound.master_volume"), settings.master_volume,
                                       T("menus.sound.master_volume_about")))
        actions.setting = GameSettingChange{GameSetting::MasterVolume, *volume};
    SettingSwitch(list, actions, T("menus.sound.mute"), settings.mute, GameSetting::Mute);
    list.Caption(T("menus.sound.sound_levels"));
    for (std::size_t n = 0; n < kSoundGroups.size(); ++n)
        if (const auto volume = VolumeBars(list, texts::SoundGroupName(kSoundGroups[n]), settings.sound_volumes[n],
                                           texts::SoundGroupAbout(kSoundGroups[n])))
            actions.setting = GameSettingChange{GameSetting::SoundVolume, *volume, int(n)};
    if (model.game.known && !model.network_match)
    {
        list.Caption(T("menus.sound.game_levels"));
        const char* about = T("menus.sound.game_levels_about");
        // Each bar 0-10, its level as the game shows it.
        const auto bar = [&](const char* label, int value, std::optional<int>& out) {
            const std::string text = std::to_string(value);
            if (const auto next = list.Bars(label, value, 0, 10, 1, 10, text.c_str(), about)) out = *next;
        };
        bar(T("menus.sound.game_music"), model.game.music, actions.music);
        bar(T("menus.sound.game_effects"), model.game.effects, actions.effects);
        bar(T("menus.sound.game_voices"), model.game.voices, actions.voices);
    }
    list.Caption(T("menus.sound.online"));
    SettingSwitch(list, actions, T("menus.sound.challenge_sound"), settings.challenge_sound,
                  GameSetting::ChallengeSound, T("menus.sound.challenge_sound_about"));
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

// The player's set (what it is, and what they can do now), wherever it is shown.
void DrawSet(const PageModel& model, List& list, PageActions& actions)
{
    const auto& set = model.overlay.online.set;
    if (set.stage == overlay::OnlinePanelSet::Stage::None) return;
    const Person opponent = PersonOf(model, set.opponent);
    std::vector<std::string> about{set.status};
    if (!set.rules.empty()) about.push_back(set.rules);
    list.Card(&opponent, T("menus.you.your_set"), set.title.c_str(), about, ToneOf(set.tone), set.games, set.won,
              set.lost);
    if (set.can_accept && list.Plate(T("menus.you.accept"))) actions.online.accept = true;
    if (set.can_decline && list.Plate(T("menus.you.decline"))) actions.online.decline = true;
    if (set.can_ready && list.Plate(T("menus.you.ready"))) actions.online.ready = true;
    if (set.can_rematch && list.Plate(T("menus.you.rematch"))) actions.online.rematch = true;
    if (set.can_cancel)
    {
        const std::string label =
            Upper(set.cancel_label.empty() ? T("menus.you.cancel_set") : set.cancel_label.c_str());
        if (list.Plate(label.c_str())) actions.online.cancel = true;
    }
}

// Without a player profile: where to make one.
void DrawNoProfile(List& list)
{
    list.Card(nullptr, nullptr, T("menus.you.no_profile_title"), {T("menus.you.no_profile")});
}

// The rows of a section of the online lists, or -1 when it is not there.
int SectionRows(const overlay::OnlinePanelModel& online, overlay::OnlinePanelSection::Id id)
{
    for (const auto& section : online.sections)
        if (section.id == id) return int(section.rows.size());
    return -1;
}

// The rule set a challenge or a search goes out with: the one used last.
const overlay::OnlinePanelPreset* ChosenPreset(const overlay::OnlinePanelModel& online)
{
    return online.presets.empty() ? nullptr : &online.presets[std::size_t(overlay::ChosenOnlinePanelPreset(online))];
}

// ONLINE: the set going on first, then where to go. While online play waits
// for an update, why first, and the game's own online stays closed.
void DrawOnline(const PageModel& model, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    const bool locked = online.update_lock != overlay::UpdateLock::None;
    if (locked) list.Note(overlay::UpdateLockNote(online.update_lock).c_str());
    if (online.signed_out) DrawNoProfile(list);
    else DrawSet(model, list, actions);
    if (list.Plate(T("menus.online.play"))) actions.open = PageId::Play;
    const char* busy = !online.signed_out && !online.me.available && online.me.can_set_available
        ? T("menus.you.do_not_disturb") : nullptr;
    if (list.Plate(T("menus.online.profile"), nullptr, true, busy)) actions.open = PageId::Profile;
    if (list.Plate(T("menus.online.leaderboard"))) actions.open = PageId::Leaderboard;
    if (model.from_wheel
        && list.Plate(T("menus.online.wii_online"), nullptr, !locked,
                      locked ? overlay::UpdateLockReason(online.update_lock).c_str() : nullptr))
        actions.wii_online = true;
}

// PLAY: against a friend, a random opponent, or RANKED; each with how it stands.
void DrawPlay(const PageModel& model, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    std::string friends, random;
    if (!online.signed_out)
    {
        if (const int count = SectionRows(online, overlay::OnlinePanelSection::Id::Online); count > 0)
            friends = texts::Count("menus.play.online_count", count);
        if (online.search.active) random = T("menus.play.searching");
        else if (!online.me.available && online.me.can_set_available) random = T("menus.you.do_not_disturb");
        else if (const int count = SectionRows(online, overlay::OnlinePanelSection::Id::Available); count > 0)
            random = texts::Count("menus.play.available_count", count);
    }
    const char* ranked = model.overlay.ranked.going ? T("menus.play.on_the_way") : nullptr;
    if (list.Plate(T("menus.play.friends"), nullptr, true, friends.c_str())) actions.open = PageId::Friends;
    if (list.Plate(T("menus.play.random"), nullptr, true, random.c_str())) actions.open = PageId::Random;
    if (list.Plate(T("menus.play.ranked"), nullptr, true, ranked)) actions.open = PageId::Ranked;
}

// A section of the online lists: a player who can be challenged now opens
// the match's setup, any other their page (why not, and the friend buttons).
void DrawSection(const PageModel& model, PageState& state, List& list, PageActions& actions,
                 overlay::OnlinePanelSection::Id id)
{
    const auto& online = model.overlay.online;
    if (online.signed_out)
    {
        list.Note(T("menus.players.no_profile"));
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
            if (list.PlayerRow(person, row.name.c_str(), row.status.c_str(), ToneOf(row.tone),
                               row.version.empty() ? nullptr : row.version.c_str(), nullptr))
            {
                if (row.can_challenge && !online.presets.empty()) OpenSetup(model, state, actions, row.key);
                else
                {
                    actions.open = PageId::Player;
                    actions.open_key = row.key;
                }
            }
        }
    }
}

// FRIENDS: the friends online, each to challenge from their page.
void DrawFriends(const PageModel& model, PageState& state, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    if (online.signed_out)
    {
        DrawNoProfile(list);
        return;
    }
    if (!online.note.empty()) list.Note(online.note.c_str());
    DrawSection(model, state, list, actions, overlay::OnlinePanelSection::Id::Online);
}

// RANDOM OPPONENT: the search (its setup: the rules, FIND NOW), Do not
// disturb, and the others available (listed while Do not disturb is off).
// Give and take: searching makes the player available, Do not disturb stops
// a search.
void DrawRandom(const PageModel& model, PageState& state, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    if (online.signed_out)
    {
        DrawNoProfile(list);
        return;
    }
    const auto& me = online.me;
    const double now = ImGui::GetTime();
    if (overlay::OnlinePanelSearchRunning(online, state.online, now))
    {
        const std::string time = online.search.active
            ? T("menus.random.search_time", {{"seconds", std::to_string(online.search.seconds)},
                                             {"asked", std::to_string(online.search.asked)}})
            : std::string();
        if (list.Plate(T("menus.you.stop_searching"), nullptr, true, time.c_str()))
        {
            actions.online.stop_search = true;
            state.online.search_sent = -100.0;
        }
    }
    else
    {
        const auto* preset = ChosenPreset(online);
        const bool can = online.can_search && preset;
        if (list.Plate(T("menus.random.find"), nullptr, can, preset ? preset->name.c_str() : nullptr))
            OpenSetup(model, state, actions, kSearchKey);
    }
    if (!online.note.empty()) list.Note(online.note.c_str());
    if (me.can_set_available)
        if (const auto on = list.Switch(T("menus.you.do_not_disturb"), !me.available))
        {
            actions.online.set_available = !*on;
            if (*on && online.search.active) actions.online.stop_search = true;
        }
    list.Caption(T("menus.random.available_caption"));
    if (SectionRows(online, overlay::OnlinePanelSection::Id::Available) < 0) list.Note(T("menus.random.closed"));
    else DrawSection(model, state, list, actions, overlay::OnlinePanelSection::Id::Available);
}

// A record as "won – lost", with the share won when games were played.
std::string GamesText(const platform::StatsRecord& record)
{
    const std::string won = std::to_string(record.games_won), lost = std::to_string(record.games_lost);
    const int decided = record.games_won + record.games_lost;
    if (decided == 0) return T("menus.profile.record", {{"won", won}, {"lost", lost}});
    const int share = int((100.0 * record.games_won / decided) + 0.5);
    return T("menus.profile.record_percent", {{"won", won}, {"lost", lost}, {"percent", std::to_string(share)}});
}

// PROFILE: the player, their record of all time, and all their friends.
void DrawProfile(const PageModel& model, PageState& state, List& list, PageActions& actions)
{
    const auto& online = model.overlay.online;
    if (online.signed_out)
    {
        DrawNoProfile(list);
        return;
    }
    const auto& me = online.me;
    const Person you = PersonOf(model, me.image, me.initials, me.tint_seed, -1, me.country);
    std::vector<std::string> lines;
    if (!me.detail.empty()) lines.push_back(me.detail);
    lines.push_back(me.available ? T("menus.you.open") : T("menus.you.do_not_disturb"));
    list.Card(&you, nullptr, me.name.c_str(), lines, me.available ? Tone::Good : Tone::Away);
    if (!online.message.empty()) list.Note(online.message.c_str());

    list.Caption(T("menus.profile.stats"));
    if (online.stats_missing) list.Note(T("menus.profile.no_stats"));
    else if (!online.stats) list.Note(T("menus.common.loading"));
    else
    {
        // RANKED opens its page (this season and today); friend matches are
        // the sets and matches between friends.
        if (const auto* ranked = online.stats->Mode("ranked"))
            if (list.Row(T("menus.profile.ranked"), GamesText(ranked->all).c_str())) actions.open = PageId::Ranked;
        if (const auto* friends = online.stats->Mode("friends"))
            list.Row(T("menus.profile.friend_matches"), GamesText(friends->all).c_str());
    }

    list.Caption(T("menus.profile.friends"));
    DrawSection(model, state, list, actions, overlay::OnlinePanelSection::Id::Friends);
}

// ADD FRIEND or REMOVE FRIEND, for friends added in the game (a Discord
// friend stays Discord's).
void FriendButton(const overlay::OnlinePanelRow& row, List& list, PageActions& actions)
{
    if (!row.can_befriend) return;
    const bool mii = row.kind == overlay::OnlinePanelRow::Kind::Mii;
    if (row.is_friend)
    {
        if (list.Plate(T("menus.player.remove_friend")))
            mii ? void(actions.online.remove_mii_friend = row.mii_profile)
                : void(actions.online.remove_friend = row.player_id);
    }
    else if (list.Plate(T("menus.player.add_friend")))
        mii ? void(actions.online.add_mii_friend = row.mii_profile) : void(actions.online.add_friend = row.player_id);
}

void DrawPlayer(const std::string& key, const PageModel& model, PageState& state, List& list, PageActions& actions)
{
    const auto* row = overlay::FindRow(model.overlay.online, key);
    if (!row)
    {
        list.Note(T("menus.player.gone"));
        return;
    }
    // Who they are and how they are.
    const Person person = PersonOf(model, *row);
    std::vector<std::string> lines{row->status};
    if (!row->detail.empty()) lines.push_back(row->detail);
    if (!row->version.empty()) lines.push_back(T("menus.player.version", {{"version", row->version}}));
    list.Card(&person, row->is_friend ? T("menus.player.friend") : T("menus.player.player"), row->name.c_str(), lines,
              ToneOf(row->tone));
    // CHALLENGE: the match's setup, once they can be challenged; greyed
    // until then, why on it.
    const auto& online = model.overlay.online;
    const bool can = row->can_challenge && !online.presets.empty();
    const char* why = can || row->blocked.empty() ? nullptr : row->blocked.c_str();
    if (list.Plate(T("menus.player.challenge"), nullptr, can, why)) OpenSetup(model, state, actions, row->key);
    FriendButton(*row, list, actions);
}

std::string GameLength(int seconds)
{
    if (seconds % 60 == 0) return T("menus.setup.minutes", {{"minutes", std::to_string(seconds / 60)}});
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
        list.Note(!search && !row ? T("menus.player.gone") : T("menus.players.no_rules"));
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
        list.Card(&person, nullptr, row->name.c_str(), {row->status}, ToneOf(row->tone));
    }
    // The way on first (the rules it goes out with on it, or why it can't go
    // now), the rules under it.
    const bool can = search ? online.can_search || searching : row->can_challenge;
    const char* label = !search ? T("menus.setup.send_challenge")
        : searching             ? T("menus.setup.stop_searching")
                                : T("menus.setup.find_now");
    const std::string going = !can && row && !row->blocked.empty() ? row->blocked
        : changed ? T("menus.setup.changed_rules", {{"rules", preset.name}})
                  : preset.name;
    if (list.Plate(label, nullptr, can, searching ? nullptr : going.c_str()))
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
                // Give and take: who looks for an opponent can be found.
                if (!online.me.available && online.me.can_set_available) actions.online.set_available = true;
            }
            else
            {
                actions.online.challenge = challenge;
                actions.back = true;
            }
        }
    }
    if (searching) return;
    list.Caption(T("menus.setup.preset"));
    if (const auto step = list.Cycle(T("menus.setup.start_from"), preset.name.c_str()))
    {
        setup.preset = (setup.preset + *step + count) % count;
        rules = online.presets[std::size_t(setup.preset)].rules;
    }
    list.Caption(T("menus.setup.match"));
    Values(list, T("menus.setup.series"), {1, 3, 5, 7}, rules.games, [](int n) { return std::to_string(n); });
    const std::vector<std::string> limits{T("menus.setup.by_time"), T("menus.setup.by_goals")};
    if (const auto picked = list.Choice(T("menus.setup.win_by"), limits, rules.limit == "goals" ? 1 : 0))
        rules.limit = *picked == 1 ? "goals" : "time";
    if (rules.limit == "goals")
        Values(list, T("menus.setup.goals"), {3, 4, 5, 6, 7, 8, 9, 10}, rules.goals,
               [](int n) { return std::to_string(n); });
    else Values(list, T("menus.setup.time"), {120, 180, 240, 300}, rules.time, GameLength);
    Choices(list, T("menus.setup.stadium"), platform::StadiumChoices(), rules.stadium);
    list.Caption(T("menus.setup.cheats"));
    Choices(list, T("menus.setup.stadium_cheat"), platform::StadiumCheatChoices(), rules.stadium_cheat);
    Choices(list, T("menus.setup.player_cheat"), platform::PlayerCheatChoices(), rules.player_cheat);
    Choices(list, T("menus.setup.powerup_cheat"), platform::PowerupCheatChoices(), rules.powerup_cheat);
    list.Caption(T("menus.setup.gameplay"));
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
    // The player challenged: a friend added in the game or not.
    if (row && row->can_befriend)
    {
        list.Caption(T("menus.player.friend"));
        FriendButton(*row, list, actions);
    }
}

std::string Record(const platform::LeaderboardEntry& entry);

// RANKED: play it, and the player's places this season and today, each
// opening its board.
void DrawRanked(const PageModel& model, List& list, PageActions& actions)
{
    const auto& ranked = model.overlay.ranked;
    if (ranked.going)
    {
        // Waiting: the player has to go back to a menu the game goes on from.
        if (ranked.waiting) list.Note(T("menus.ranked.waiting"));
        if (list.Plate(T("menus.ranked.stop"), nullptr, true, T("menus.play.on_the_way"))) actions.ranked.stop = true;
    }
    else
    {
        // Greyed when it can't go now, why on it.
        const char* why = ranked.can_play || ranked.blocked.empty() ? nullptr : ranked.blocked.c_str();
        if (list.Plate(T("menus.ranked.play"), nullptr, ranked.can_play, why)) actions.ranked.play = true;
    }
    if (ranked.boards && !ranked.signed_out)
    {
        list.Caption(T("menus.ranked.your_place"));
        // A row without a game (the game puts an empty one) is no place.
        const auto place = [](const platform::LeaderboardBoard& board) {
            const auto& you = board.you;
            if (!you || you->rank <= 0 || (you->points == 0 && you->wins.value_or(0) + you->losses.value_or(0) == 0))
                return std::string(T("menus.ranked.not_placed"));
            return texts::Count("menus.ranked.place", board.you->points,
                                {{"place", std::to_string(board.you->rank)}, {"record", Record(*board.you)}});
        };
        const std::string season = place(ranked.boards->season), today = place(ranked.boards->daily);
        if (list.Row(T("menus.ranked.season"), season.c_str()))
        {
            actions.open = PageId::Leaderboard;
            actions.open_category = 0;
        }
        if (list.Row(T("menus.ranked.today"), today.c_str()))
        {
            actions.open = PageId::Leaderboard;
            actions.open_category = 1;
        }
    }
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
        list.Note(T("menus.leaderboard.no_profile"));
        return;
    }
    if (ranked.missing)
    {
        list.Note(T("menus.leaderboard.missing"));
        return;
    }
    if (!ranked.boards)
    {
        list.Note(T("menus.common.loading"));
        return;
    }
    const bool daily = category == 1;
    const auto& board = daily ? ranked.boards->daily : ranked.boards->season;
    const std::string days = board.first_day == board.last_day || board.last_day.empty()
        ? board.first_day : board.first_day + "  \xE2\x80\x93  " + board.last_day;
    const std::string players = texts::Count("menus.leaderboard.players", board.players);
    const std::string summary = T("menus.leaderboard.summary", {{"days", days}, {"players", players}});
    list.Note(summary.c_str());
    if (board.top.empty())
        list.Note(daily ? T("menus.leaderboard.nobody_today") : T("menus.leaderboard.nobody"));
    bool youShown = false;
    for (const auto& entry : board.top)
    {
        const std::string name = entry.player.empty() ? entry.mii : entry.player;
        list.BoardRow(entry.rank, name.c_str(), Record(entry).c_str(), std::to_string(entry.points).c_str(), entry.you);
        youShown = youShown || entry.you;
    }
    if (board.you && !youShown)
    {
        list.Caption(T("menus.leaderboard.you_caption"));
        const auto& you = *board.you;
        const std::string name = you.player.empty() ? you.mii : you.player;
        list.BoardRow(you.rank, name.c_str(), Record(you).c_str(), std::to_string(you.points).c_str(), true);
    }
}

void DrawMatch(List& list, PageActions& actions)
{
    if (list.Plate(T("menus.match.continue"))) actions.close = true;
    if (list.Plate(T("menus.match.forfeit"))) actions.open = PageId::Forfeit;
}

void DrawForfeit(const PageModel& model, List& list, PageActions& actions)
{
    if (model.forfeiting)
    {
        list.Note(T("menus.forfeit.leaving"));
        return;
    }
    list.Note(model.set_match ? T("menus.forfeit.set_note") : T("menus.forfeit.match_note"));
    if (list.Plate(T("menus.forfeit.forfeit"))) actions.forfeit = true;
    if (list.Plate(T("menus.common.cancel"))) actions.back = true;
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
    case PageId::Menu: return T("menus.menu.title");
    case PageId::Options: return T("menus.options.title");
    case PageId::Gameplay: return T("menus.gameplay.title");
    case PageId::Settings: return T("menus.settings.title");
    case PageId::Quit: return T("menus.quit.title");
    case PageId::Online: return T("menus.online.title");
    case PageId::Play: return T("menus.play.title");
    case PageId::Friends: return T("menus.friends.title");
    case PageId::Random: return T("menus.random.title");
    case PageId::Profile: return T("menus.profile.title");
    case PageId::Player:
        if (const auto* row = overlay::FindRow(model.overlay.online, key)) return row->name;
        return T("menus.player.title");
    case PageId::Setup: return key == kSearchKey ? T("menus.setup.title_random") : T("menus.setup.title_challenge");
    case PageId::Ranked: return T("menus.ranked.title");
    case PageId::Leaderboard: return T("menus.leaderboard.title");
    case PageId::Prompt:
        if (model.prompt && !model.prompt->kicker.empty()) return model.prompt->kicker;
        return T("menus.prompt.title");
    case PageId::Match: return T("menus.match.title");
    case PageId::Forfeit: return T("menus.forfeit.title");
    }
    return "";
}

std::vector<std::string> PageCategories(PageId id)
{
    switch (id)
    {
    case PageId::Gameplay:
    {
        std::vector<std::string> names;
        for (const auto& group : overlay::GameOptionGroups()) names.push_back(Upper(group.short_title));
        return names;
    }
    case PageId::Settings:
        return {T("menus.settings.display"), T("menus.settings.sound"), T("menus.settings.advanced")};
    case PageId::Leaderboard: return {T("menus.leaderboard.season"), T("menus.leaderboard.today")};
    default: return {};
    }
}

bool PageWide(PageId id)
{
    // All but the hubs and the questions: room for a page's lists and for
    // the facts on its buttons.
    switch (id)
    {
    case PageId::Menu:
    case PageId::Options:
    case PageId::Online:
    case PageId::Quit:
    case PageId::Prompt:
    case PageId::Match:
    case PageId::Forfeit: return false;
    default: return true;
    }
}

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
    case PageId::Play: DrawPlay(model, list, actions); break;
    case PageId::Friends: DrawFriends(model, state, list, actions); break;
    case PageId::Random: DrawRandom(model, state, list, actions); break;
    case PageId::Profile: DrawProfile(model, state, list, actions); break;
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
