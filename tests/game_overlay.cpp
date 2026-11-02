// The game's overlay (src/overlay) in a headless ImGui frame loop: where the
// keyboard and gamepad cursor lands as a screen or tab opens and as one comes
// back, A and Enter on a row (its player's screen) and on its buttons, B and
// Backspace (back, or close on a first screen), Escape, the tab keys (L1/R1,
// L2/R2, Q/E, Ctrl+Tab), a click on a tab or beside the window, each tab's own
// screens, Game options' switches and the frame's Done; tabs from data (more
// tabs, hidden and unavailable ones skipped, ids not places, badges) and the
// strip from narrow to wide. No renderer: the frames are built and thrown away.

#include "overlay/game_overlay.h"
#include "overlay/overlay_prompt.h"
#include "overlay/overlay_unit.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstdio>
#include <functional>

namespace platform = mscharged::platform;
using namespace mscharged::overlay;
using mscharged::GameTweak;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (ok) return;
    std::fprintf(stderr, "game_overlay.cpp:%d: %s\n", line, what);
    ++failures;
}
#define CHECK(x) Check((x), #x, __LINE__)

// What several frames reported, together.
void Merge(GameOverlayActions& all, const GameOverlayActions& a)
{
    all.close |= a.close;
    auto& o = all.online;
    o.accept |= a.online.accept;
    o.decline |= a.online.decline;
    o.ready |= a.online.ready;
    o.cancel |= a.online.cancel;
    o.rematch |= a.online.rematch;
    if (a.online.challenge) o.challenge = a.online.challenge;
    if (a.online.search) o.search = a.online.search;
    o.stop_search |= a.online.stop_search;
    if (a.online.add_friend) o.add_friend = a.online.add_friend;
    if (a.online.remove_friend) o.remove_friend = a.online.remove_friend;
    if (a.online.add_mii_friend) o.add_mii_friend = a.online.add_mii_friend;
    if (a.online.remove_mii_friend) o.remove_mii_friend = a.online.remove_mii_friend;
    if (a.online.set_available) o.set_available = a.online.set_available;
    if (a.online.set_challenge_rules) o.set_challenge_rules = a.online.set_challenge_rules;
    if (a.set_option) all.set_option = a.set_option;
    if (a.set_setting) all.set_setting = a.set_setting;
}

// One ImGui context of a 1280x720 window, with keyboard and gamepad navigation.
struct Harness
{
    GameOverlayModel model;
    GameOverlayState state;
    float unit = 1.0f; // the style's

    Harness()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = {1280, 720};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_HasMouseCursors;
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(ImTextureID(1));
        io.AddMousePosEvent(-1, -1); // away from the overlay until a test moves it
    }
    ~Harness() { ImGui::DestroyContext(); }

    GameOverlayActions Frame(const std::function<void(ImGuiIO&)>& input = {})
    {
        if (input) input(ImGui::GetIO());
        ImGui::NewFrame();
        GameOverlayActions actions;
        GameOverlayStyle style;
        style.unit = unit;
        style.image = [](const std::string&) { return ImTextureID(0); };
        DrawGameOverlay(model, state, actions, style);
        ImGui::Render();
        return actions;
    }

    // The overlay opened: a few frames for the cursor to land.
    void Open(OverlayTab tab = OverlayTab::Online)
    {
        state.Open(tab);
        Frame();
        Frame();
    }

    // A key (with modifiers) pressed and released over a few frames.
    GameOverlayActions Press(ImGuiKey key, bool ctrl = false, bool shift = false)
    {
        GameOverlayActions all;
        Merge(all, Frame([&](ImGuiIO& io) {
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
            io.AddKeyEvent(key, true);
        }));
        Merge(all, Frame([&](ImGuiIO& io) {
            io.AddKeyEvent(key, false);
            if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        }));
        Merge(all, Frame());
        return all;
    }

    // A click at (x, y).
    GameOverlayActions Click(float x, float y)
    {
        GameOverlayActions all;
        Merge(all, Frame([&](ImGuiIO& io) { io.AddMousePosEvent(x, y); }));
        Merge(all, Frame([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); }));
        Merge(all, Frame([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); }));
        Merge(all, Frame([](ImGuiIO& io) { io.AddMousePosEvent(-1, -1); }));
        return all;
    }

    bool OnPlayer(const char* key) const
    {
        return !state.online.Empty() && state.online.Top().id == int(OnlinePanelScreen::Player)
            && state.online.Top().key == key;
    }
    bool OnMain() const { return !state.online.Empty() && state.online.Top().id == int(OnlinePanelScreen::Main); }
    bool OnSetup(const char* key) const
    {
        return !state.online.Empty() && state.online.Top().id == int(OnlinePanelScreen::Setup)
            && state.online.Top().key == key;
    }
};

platform::ProfilePlayer Online(std::int64_t id, const char* name)
{
    platform::ProfilePlayer player;
    player.id = id;
    player.name = name;
    player.online = true;
    player.activity = "launcher";
    player.is_friend = true;
    return player;
}

OnlinePanelModel Model(const std::optional<platform::ProfileMatch>& match = std::nullopt)
{
    platform::PlayerLists lists;
    lists.friends = {Online(10, "Mario"), Online(11, "Peach")};
    OnlinePanelContext context;
    context.me.id = 1;
    context.me.name = "Anosch";
    platform::RuleSet competitive;
    competitive.id = 1;
    competitive.name = "Competitive";
    competitive.rules.limit = "time";
    competitive.rules.time = 180;
    competitive.rules.games = 3;
    competitive.rules.stadium = "random";
    context.rule_sets = {competitive};
    return MakeOnlinePanelModel(lists, match, context);
}

void TestScreens()
{
    Harness h;
    h.model.online = Model();
    h.Open();
    CHECK(h.state.Tab() == OverlayTab::Online && h.OnMain());
    // A row opens its player; their screen's cursor is on Challenge, which
    // opens the challenge's setup; its cursor is on Send challenge (beside
    // the rules), which challenges by the server's rule set as it is and goes
    // back to the list.
    auto actions = h.Press(ImGuiKey_Enter);
    CHECK(!actions.Any());
    CHECK(h.OnPlayer("p10"));
    actions = h.Press(ImGuiKey_Enter);
    CHECK(!actions.online.challenge && h.OnSetup("p10"));
    actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.online.challenge && actions.online.challenge->player == 10 && actions.online.challenge->rule_set == 1
          && actions.online.challenge->name == "Competitive" && actions.online.set_challenge_rules == "Competitive");
    CHECK(h.OnMain());
    // Back on the list the cursor is on Mario again: Enter opens him again.
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p10"));
    // Backspace goes back, without closing.
    actions = h.Press(ImGuiKey_Backspace);
    CHECK(!actions.close && h.OnMain());
    // Down: Peach; her screen; right of Challenge her friend button.
    h.Press(ImGuiKey_DownArrow);
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p11"));
    h.Press(ImGuiKey_RightArrow);
    actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.online.remove_friend && *actions.online.remove_friend == 11);
    CHECK(!actions.online.challenge && h.OnPlayer("p11"));
    // B goes back; B on the first screen closes.
    actions = h.Press(ImGuiKey_GamepadFaceRight);
    CHECK(!actions.close && h.OnMain());
    actions = h.Press(ImGuiKey_GamepadFaceRight);
    CHECK(actions.close);
    // Escape closes from a later screen too.
    h.Open();
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p10"));
    actions = h.Press(ImGuiKey_Escape);
    CHECK(actions.close);
}

void TestGamepadSet()
{
    platform::ProfileMatch set;
    set.id = 5;
    set.state = "challenged";
    set.side = "challenged";
    set.stage = "invited";
    set.opponent = Online(12, "Daisy");
    set.opponent_online = true;
    set.rules.games = 3;
    Harness h;
    h.model.online = Model(set);
    h.Open();
    // A challenge to answer: Accept has the cursor, A accepts; right of it Decline.
    auto actions = h.Press(ImGuiKey_GamepadFaceDown);
    CHECK(actions.online.accept && !actions.online.decline && h.OnMain());
    h.Press(ImGuiKey_GamepadDpadRight);
    actions = h.Press(ImGuiKey_GamepadFaceDown);
    CHECK(actions.online.decline && !actions.online.accept);
}

void TestTabs()
{
    Harness h;
    h.model.online = Model();
    h.Open();
    // Every way to the next tab (Events) and back.
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.Press(ImGuiKey_Q);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_GamepadR1);
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.Press(ImGuiKey_GamepadL1);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_GamepadR2);
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.Press(ImGuiKey_GamepadL2);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_Tab, true);
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.Press(ImGuiKey_Tab, true, true);
    CHECK(h.state.Tab() == OverlayTab::Online);
    // Four tabs: next from the last is the first, previous from the first the last.
    h.Press(ImGuiKey_E);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Settings);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_Q);
    CHECK(h.state.Tab() == OverlayTab::Settings);
    h.Press(ImGuiKey_E);
    // Clicks on the tabs at the top, from the left: each one of them opens
    // (Online at the left, Settings at the right), never closing the overlay.
    std::vector<OverlayTab> clicked;
    for (float x = 200; x < 1080; x += 20)
    {
        const auto actions = h.Click(x, 86);
        CHECK(!actions.close);
        if (clicked.empty() || clicked.back() != h.state.Tab()) clicked.push_back(h.state.Tab());
    }
    CHECK(clicked.size() == 4 && clicked[0] == OverlayTab::Online && clicked[1] == OverlayTab::Events
          && clicked[2] == OverlayTab::GameOptions && clicked[3] == OverlayTab::Settings);
    h.Click(400, 86);
    CHECK(h.state.Tab() == OverlayTab::Online);

    // Each tab keeps its screens while the overlay is open.
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p10"));
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.Press(ImGuiKey_Q);
    CHECK(h.state.Tab() == OverlayTab::Online && h.OnPlayer("p10"));
    // ... and B there goes back to the list, not to the other tab.
    auto back = h.Press(ImGuiKey_GamepadFaceRight);
    CHECK(!back.close && h.OnMain() && h.state.Tab() == OverlayTab::Online);
    // Opening again starts on the tab asked for, on its first screen.
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p10"));
    h.Open(OverlayTab::Online);
    CHECK(h.OnMain());
    h.Open(OverlayTab::GameOptions);
    CHECK(h.state.Tab() == OverlayTab::GameOptions && h.OnMain());
    // The host's own switch keys.
    h.state.Switch(OverlayTab::Online);
    h.Frame();
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.state.NextTab();
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.state.PreviousTab();
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Open(OverlayTab::Events);
    CHECK(h.state.Tab() == OverlayTab::Events && h.OnMain());
    // B on its only screen closes, as on any tab's first screen.
    CHECK(h.Press(ImGuiKey_GamepadFaceRight).close);
}

void TestGameOptions()
{
    Harness h;
    h.model.online = Model();
    h.model.options.in_match = true;
    h.Open(OverlayTab::GameOptions);
    // The cursor on the first option (Fast terrain, off by default): Enter switches it on.
    auto actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.set_option && actions.set_option->tweak == GameTweak::FastTerrain && actions.set_option->on);
    // The next one down is Secure stadiums; A on it.
    h.Press(ImGuiKey_DownArrow);
    actions = h.Press(ImGuiKey_GamepadFaceDown);
    CHECK(actions.set_option && actions.set_option->tweak == GameTweak::SecureStadiums);
    CHECK(h.state.options_state.shown && *h.state.options_state.shown == GameTweak::SecureStadiums);
    // A switched-on option switches off.
    h.model.options.tweaks[mscharged::GameTweakIndex(GameTweak::SecureStadiums)] = true;
    h.Frame();
    actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.set_option && actions.set_option->tweak == GameTweak::SecureStadiums && !actions.set_option->on);
    // B on the tab's only screen closes.
    CHECK(h.Press(ImGuiKey_GamepadFaceRight).close);

    // Which options the game offers.
    CHECK(GameOption(GameTweak::FairBallContests) && GameOption(GameTweak::MatchPoint));
    CHECK(GameOption(GameTweak::FastMenus));
    CHECK(GameOption(GameTweak::CpuVsCpu) && GameOption(GameTweak::MirrorMatch) && GameOption(GameTweak::BluePeach));
    CHECK(!GameOption(GameTweak::SkipIntro) && !GameOption(GameTweak::UnlockAll));
    CHECK(!GameOption(GameTweak::RobotGoalies) && !GameOption(GameTweak::SuperTeam) && !GameOption(GameTweak::NoHud));
    CHECK(GameOptionAtOnce(GameTweak::FastMenus) && !GameOptionAtOnce(GameTweak::NoMegaStrikes));
}

void TestMouse()
{
    Harness h;
    h.model.online = Model();
    h.Open();
    // A click in the window doesn't close it; one beside it does.
    CHECK(!h.Click(640, 600).close);
    CHECK(h.Click(60, 400).close);
}

// The window shrinks to one column: the cursor lands on the first row again.
void TestResize()
{
    Harness h;
    h.model.online = Model();
    h.Open();
    h.Frame([](ImGuiIO& io) { io.DisplaySize = {860, 640}; });
    h.Frame();
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p10"));
}

// The lists come after the overlay opened: the cursor, put on what there was
// (the switch, or Close), moves to the first row, unless the player moved it.
void TestLateLists()
{
    platform::PlayerLists none;
    OnlinePanelContext loading;
    loading.me.id = 1;
    loading.me.name = "Anosch";
    loading.loading = true;
    for (const bool random_games : {false, true})
    {
        Harness h;
        loading.random_games = random_games; // without it, nothing but Close to put the cursor on
        h.model.online = MakeOnlinePanelModel(none, std::nullopt, loading);
        h.Open();
        h.model.online = Model();
        h.Frame();
        h.Press(ImGuiKey_Enter);
        CHECK(h.OnPlayer("p10"));
    }
    // The player moved the cursor (to Close) before the lists came: it stays.
    Harness h;
    loading.random_games = true;
    h.model.online = MakeOnlinePanelModel(none, std::nullopt, loading);
    h.Open();
    h.Press(ImGuiKey_DownArrow);
    h.model.online = Model();
    h.Frame();
    const auto actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.close && h.OnMain());
}

// The model with the server's rule sets, the player's challenges by `chosen`.
OnlinePanelModel RulesModel(std::vector<std::string> names, const std::string& chosen)
{
    // Friends beside the player's card all down its height.
    platform::PlayerLists lists;
    lists.friends = {Online(10, "Mario"), Online(11, "Peach"), Online(12, "Daisy"), Online(13, "Luigi"),
                     Online(14, "Wario"), Online(15, "Yoshi")};
    OnlinePanelContext context;
    context.me.id = 1;
    context.me.name = "Anosch";
    for (std::size_t n = 0; n < names.size(); ++n)
    {
        platform::RuleSet rule_set;
        rule_set.id = std::int64_t(n + 1);
        rule_set.name = names[n];
        rule_set.rules.skill = "rookie";
        rule_set.rules.limit = "time";
        rule_set.rules.time = 180;
        rule_set.rules.games = 3;
        rule_set.rules.stadium = "random";
        context.rule_sets.push_back(rule_set);
    }
    context.chosen_rules = chosen;
    return MakeOnlinePanelModel(lists, std::nullopt, context);
}

// A challenge's setup: it opens with the preset chosen last; left and right
// (and A) on a rule go through its values; Send challenge sends the changed
// rules (no rule set), named after the preset, and remembers the preset.
void TestChallengeSetup()
{
    const std::vector<std::string> names{"Competitive", "Classic", "First to 5"};
    Harness h;
    h.model.online = RulesModel(names, "Competitive");
    h.Open();
    // Daisy first (by name).
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p12"));
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnSetup("p12") && h.state.online_state.preset == 0 && h.state.online_state.rules.games == 3);
    // Right from Send challenge: the preset beside it, unchanged.
    CHECK(!h.Press(ImGuiKey_RightArrow).online.Any() && h.state.online_state.preset == 0);
    // The preset: Classic, its rules; left goes back.
    CHECK(!h.Press(ImGuiKey_RightArrow).online.Any());
    CHECK(h.state.online_state.preset == 1 && h.state.online_state.rules == h.model.online.presets[1].rules);
    h.Press(ImGuiKey_LeftArrow);
    h.Press(ImGuiKey_LeftArrow);
    CHECK(h.state.online_state.preset == 2);
    h.Press(ImGuiKey_RightArrow);
    h.Press(ImGuiKey_RightArrow);
    // The series: best of 5, of 7; A the next (one game, round).
    h.Press(ImGuiKey_DownArrow);
    h.Press(ImGuiKey_RightArrow);
    CHECK(h.state.online_state.rules.games == 5);
    h.Press(ImGuiKey_GamepadDpadRight);
    CHECK(h.state.online_state.rules.games == 7);
    h.Press(ImGuiKey_GamepadFaceDown);
    CHECK(h.state.online_state.rules.games == 1 && h.OnSetup("p12"));
    h.Press(ImGuiKey_LeftArrow);
    CHECK(h.state.online_state.rules.games == 7);
    // Y sends from the rules (F as well).
    const auto actions = h.Press(ImGuiKey_GamepadFaceUp);
    CHECK(actions.online.challenge && actions.online.challenge->player == 12 && actions.online.challenge->rule_set == 0
          && actions.online.challenge->name == "Classic (changed)" && actions.online.challenge->rules.games == 7);
    CHECK(actions.online.set_challenge_rules == "Classic" && h.OnMain());
    // Back says nothing: the setup's B goes to the player.
    h.Press(ImGuiKey_Enter);
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnSetup("p12"));
    CHECK(!h.Press(ImGuiKey_GamepadFaceRight).online.challenge && h.OnPlayer("p12"));
}

// A search's setup: Find an opponent (beside the rules, the cursor on it)
// sends a search, no player, by the server's rule set as it is, and the
// screen shows it running: Stop searching, the rules not to be changed.
void TestSearchSetup()
{
    Harness h;
    h.model.online = RulesModel({"Competitive", "Classic"}, "Classic");
    h.model.online.can_search = true;
    h.Open();
    h.state.online_state.setup_for = "search";
    h.state.online_state.preset = 1;
    h.state.online_state.rules = h.model.online.presets[1].rules;
    h.state.online.Push({int(OnlinePanelScreen::Setup), "search", "Find an opponent"}, 0);
    h.Frame();
    h.Frame();
    auto actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.online.search && actions.online.search->player == 0 && actions.online.search->name == "Classic"
          && actions.online.search->rule_set == 2 && !actions.online.challenge);
    CHECK(h.OnSetup("search"));
    // The server has it: the rules stay, F stops it.
    h.model.online.search.active = true;
    h.model.online.can_search = false;
    h.Frame();
    h.Press(ImGuiKey_RightArrow);
    h.Press(ImGuiKey_RightArrow);
    CHECK(h.state.online_state.preset == 1 && h.state.online_state.rules == h.model.online.presets[1].rules);
    actions = h.Press(ImGuiKey_F);
    CHECK(actions.online.stop_search && !actions.online.search && h.OnSetup("search"));
}

// The Settings tab: one screen, a section always shown (Display first); the
// menu's cursor shows its section, right goes into its rows (volumes are
// sliders: five points a step), left comes back to its menu item, B closes.
void TestSettings()
{
    Harness h;
    h.model.online = Model();
    h.model.settings.master_volume = 50;
    h.model.settings.resolutions = {"Native", "2x", "Window"};
    h.model.settings.resolution = 2;
    h.Open(OverlayTab::Settings);
    CHECK(h.state.Tab() == OverlayTab::Settings && h.state.settings.Top().id == int(GameSettingsScreen::Menu));
    CHECK(h.state.settings_state.section == 0 && !h.state.settings.CanGoBack());
    h.Press(ImGuiKey_DownArrow);
    CHECK(h.state.settings_state.section == 1);
    // Into Sound: Master volume, a slider.
    auto actions = h.Press(ImGuiKey_RightArrow);
    CHECK(!actions.set_setting && h.state.settings_state.section == 1);
    actions = h.Press(ImGuiKey_RightArrow);
    CHECK(actions.set_setting && actions.set_setting->setting == GameSetting::MasterVolume
          && actions.set_setting->value == 55 && !actions.set_setting->live);
    actions = h.Press(ImGuiKey_LeftArrow);
    CHECK(actions.set_setting && actions.set_setting->value == 45);
    h.Press(ImGuiKey_DownArrow);
    actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.set_setting && actions.set_setting->setting == GameSetting::Mute && actions.set_setting->value == 1);
    // A sound level: the second of its group (Effects after Music).
    h.Press(ImGuiKey_DownArrow);
    h.Press(ImGuiKey_DownArrow);
    actions = h.Press(ImGuiKey_LeftArrow);
    CHECK(actions.set_setting && actions.set_setting->setting == GameSetting::SoundVolume
          && actions.set_setting->group == 1 && actions.set_setting->value == 95);
    // Up to Mute, a switch: left goes back to the menu, on Sound (not the
    // item nearest), and its section stays.
    h.Press(ImGuiKey_UpArrow);
    h.Press(ImGuiKey_UpArrow);
    h.Press(ImGuiKey_LeftArrow);
    h.Frame();
    CHECK(h.state.settings_state.section == 1);
    // Display: the 3D resolution goes no further than its last.
    h.Press(ImGuiKey_UpArrow);
    CHECK(h.state.settings_state.section == 0);
    h.Press(ImGuiKey_RightArrow);
    h.Press(ImGuiKey_DownArrow);
    actions = h.Press(ImGuiKey_RightArrow);
    CHECK(actions.set_setting && actions.set_setting->setting == GameSetting::Resolution
          && actions.set_setting->value == 2);
    // B closes: there is nothing to go back to.
    actions = h.Press(ImGuiKey_GamepadFaceRight);
    CHECK(actions.close);
}

// The Settings tab as a player gets there: Online first, E to Settings,
// Display shown, right into it, down to Performance numbers.
void TestSettingsByKeys()
{
    {
        Harness g;
        g.model.online = Model();
        g.model.settings.resolutions = {"Native", "Window"};
        g.model.settings.pictures = {"Keep its shape", "Stretch to fill"};
        g.Open();
        for (int n = 0; n < 3; ++n) g.Press(ImGuiKey_E);
        CHECK(g.state.Tab() == OverlayTab::Settings && g.state.settings_state.section == 0);
        g.Frame();
        g.Press(ImGuiKey_RightArrow);
        for (int n = 0; n < 3; ++n) g.Press(ImGuiKey_DownArrow);
        const auto toggled = g.Press(ImGuiKey_Enter);
        CHECK(toggled.set_setting && toggled.set_setting->setting == GameSetting::PerformanceNumbers);
    }
}

// Without a player profile: the Online tab only invites to make one (nothing
// to press), and the other tabs work as ever.
void TestSignedOut()
{
    Harness h;
    h.model.online.signed_out = true;
    h.model.settings.master_volume = 50;
    h.Open();
    CHECK(h.state.Tab() == OverlayTab::Online);
    GameOverlayActions actions;
    for (const ImGuiKey key : {ImGuiKey_DownArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadFaceUp, ImGuiKey_F})
    {
        actions = h.Press(key);
        CHECK(!actions.online.Any() && h.state.Tab() == OverlayTab::Online);
    }
    for (int n = 0; n < 3; ++n) h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Settings);
    h.Press(ImGuiKey_DownArrow);
    h.Press(ImGuiKey_RightArrow);
    actions = h.Press(ImGuiKey_RightArrow);
    CHECK(actions.set_setting && actions.set_setting->setting == GameSetting::MasterVolume
          && actions.set_setting->value == 55);
}

// The overlay's scale: the picture's height / 900, but in small windows
// never so small that its captions (13 dp) are under 11 pixels, as long as its
// smallest window fits; and its window then takes more of the picture.
void TestSmallWindows()
{
    CHECK(OverlayUnit(1920, 1080) == 1.2f && OverlayUnit(3840, 2160) == 2.4f && OverlayUnit(2560, 1080) == 1.2f);
    CHECK(OverlayUnit(7680, 4320) == 4.0f);
    // At 1280x720 the captions keep their 11 pixels.
    CHECK(OverlayUnit(1280, 720) == 11.0f / 13.0f);
    for (const auto& [width, height] : {std::pair{640.0f, 448.0f}, std::pair{800.0f, 450.0f}, std::pair{854.0f, 480.0f},
                                        std::pair{1024.0f, 576.0f}, std::pair{960.0f, 540.0f}})
    {
        const float unit = OverlayUnit(width, height);
        CHECK(std::round(13.0f * unit) >= 11.0f);                           // the captions as AddOnlinePanelFonts makes them
        CHECK(672.0f * unit <= width + 0.01f && 452.0f * unit <= height + 0.01f); // the smallest window fits
    }
    CHECK(OverlayUnit(800, 450) > 450.0f / 900.0f);
    // Too small for both: it fits, down to 0.5.
    CHECK(OverlayUnit(480, 360) < 11.0f / 13.0f && 672.0f * OverlayUnit(480, 360) <= 480.01f);
    CHECK(OverlayUnit(320, 240) == 0.5f);

    // In a 640x448 window, the overlay's window reaches near its edges: a
    // click 20 pixels in is on it (it doesn't close), one in the corner beside it.
    Harness h;
    h.model.online = Model();
    h.Frame([](ImGuiIO& io) { io.DisplaySize = {640, 448}; });
    h.unit = OverlayUnit(640, 448);
    h.Open();
    CHECK(!h.Click(20, 224).close && !h.Click(620, 224).close && !h.Click(320, 428).close);
    CHECK(h.Click(3, 3).close);
}

// The prompt after a set or for a challenge: A, Enter and clicks on its
// buttons, B, Escape and a click beside it; the cursor on the primary button
// as it opens.
void TestPrompt()
{
    Harness h;
    OverlayPrompt prompt;
    prompt.kind = OverlayPrompt::Kind::SetOver;
    prompt.kicker = "SET OVER";
    prompt.title = "You won the set";
    prompt.opponent.name = "Koopa";
    prompt.opponent.initials = "K";
    prompt.has_score = true;
    prompt.won = 2;
    prompt.lost = 1;
    prompt.games = 3;
    prompt.primary = "Rematch";
    prompt.secondary = "Not now";
    OverlayPromptState state;
    const auto frame = [&](const std::function<void(ImGuiIO&)>& input = {}) {
        if (input) input(ImGui::GetIO());
        ImGui::NewFrame();
        OverlayPromptActions actions;
        DrawOverlayPrompt(prompt, state, actions, OverlayStyle{});
        ImGui::Render();
        return actions;
    };
    const auto press = [&](ImGuiKey key) {
        OverlayPromptActions all;
        for (const auto& a : {frame([key](ImGuiIO& io) { io.AddKeyEvent(key, true); }),
                              frame([key](ImGuiIO& io) { io.AddKeyEvent(key, false); }), frame()})
        {
            all.primary |= a.primary;
            all.secondary |= a.secondary;
        }
        return all;
    };
    const auto click = [&](float x, float y) {
        OverlayPromptActions all;
        for (const auto& a : {frame([x, y](ImGuiIO& io) { io.AddMousePosEvent(x, y); }),
                              frame([](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); }),
                              frame([](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); }),
                              frame([](ImGuiIO& io) { io.AddMousePosEvent(-1, -1); })})
        {
            all.primary |= a.primary;
            all.secondary |= a.secondary;
        }
        return all;
    };
    // Its entrance (a fifth of a second), then the keys.
    state.Open();
    for (int i = 0; i < 20; ++i) frame();
    auto actions = press(ImGuiKey_Enter);
    CHECK(actions.primary && !actions.secondary);
    actions = press(ImGuiKey_GamepadFaceDown);
    CHECK(actions.primary && !actions.secondary);
    actions = press(ImGuiKey_GamepadFaceRight);
    CHECK(actions.secondary && !actions.primary);
    actions = press(ImGuiKey_Escape);
    CHECK(actions.secondary && !actions.primary);
    // Left: the secondary button, which A presses; opened again, the primary has the cursor.
    press(ImGuiKey_GamepadDpadLeft);
    actions = press(ImGuiKey_GamepadFaceDown);
    CHECK(actions.secondary && !actions.primary);
    state.Open();
    for (int i = 0; i < 20; ++i) frame();
    actions = press(ImGuiKey_Enter);
    CHECK(actions.primary && !actions.secondary);
    // Clicks (the default font: the prompt is 560 wide and 251 tall in the
    // middle of 1280x720, its buttons 48 tall from y 409.5).
    actions = click(760, 430);
    CHECK(actions.primary && !actions.secondary);
    actions = click(500, 430);
    CHECK(actions.secondary && !actions.primary);
    actions = click(640, 270);
    CHECK(!actions.primary && !actions.secondary);
    actions = click(100, 100);
    CHECK(actions.secondary && !actions.primary);
}

// The frame alone: a screen with Done, and nothing else to put the cursor on.
void TestFrameDone()
{
    Harness h;
    OverlayScreens screens;
    screens.Open({7, {}, "Rules"});
    bool done = false, closed = false;
    const auto frame = [&](const std::function<void(ImGuiIO&)>& input) {
        if (input) input(ImGui::GetIO());
        ImGui::NewFrame();
        OverlayStyle style;
        BeginOverlay(style, screens, "Rules");
        OverlayFooter footer;
        footer.done = "Done";
        const OverlayResult result = EndOverlay(footer);
        done |= result.done;
        closed |= result.close;
        ImGui::Render();
    };
    frame({});
    frame({});
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, true); });
    frame([](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Enter, false); });
    frame({});
    CHECK(done && !closed);
}
} // namespace

// Tabs from data: more tabs between Online and Game options, the keys over
// the hidden and unavailable ones and round, the host's switch by id, badges.
void TestMoreTabs()
{
    const auto tab = [](int id, const char* label, bool available = true, bool hidden = false) {
        GameOverlayMoreTab more;
        more.tab.id = id;
        more.tab.label = label;
        more.tab.icon = mscharged::launcher::Icon::Trophy;
        more.tab.available = available;
        more.tab.hidden = hidden;
        return more;
    };
    // The strip's own stepping.
    std::vector<OverlayTabs::Tab> strip(4);
    for (int n = 0; n < 4; ++n) strip[n].id = 10 + n;
    strip[1].available = false;
    strip[2].hidden = true;
    CHECK(OverlayTabStep(strip, 10, 1) == 13 && OverlayTabStep(strip, 13, 1) == 10);
    CHECK(OverlayTabStep(strip, 10, -1) == 13 && OverlayTabStep(strip, 13, -1) == 10);
    CHECK(OverlayTabStep(strip, 99, 1) == 10 && OverlayTabStep(strip, 99, -1) == 13);
    CHECK(OverlayTabOpens(strip, 10) && !OverlayTabOpens(strip, 11) && !OverlayTabOpens(strip, 12));
    CHECK(!OverlayTabOpens(strip, 99));
    strip[3].available = false;
    CHECK(OverlayTabStep(strip, 10, 1) == 10); // nothing else opens

    Harness h;
    platform::ProfileMatch set;
    set.id = 5;
    set.state = "challenged";
    set.side = "challenged";
    set.stage = "invited";
    set.opponent = Online(12, "Daisy");
    set.opponent_online = true;
    h.model.online = Model(set);
    int drawn = 0;
    h.model.more_tabs = {tab(100, "Ranked"), tab(101, "Tournaments", false), tab(102, "Events", true, true),
                         tab(103, "Leaderboard")};
    h.model.more_tabs[0].draw = [&drawn](const OverlayLayout&) { ++drawn; };
    // Online first, Events, Game options and Settings last, the more tabs
    // between; Online's badge: the challenge.
    const auto tabs = GameOverlayTabs(h.model);
    CHECK(tabs.size() == 8 && tabs.front().id == int(OverlayTab::Online));
    CHECK(tabs.back().id == int(OverlayTab::Settings) && tabs[6].id == int(OverlayTab::GameOptions)
          && tabs[5].id == int(OverlayTab::Events));
    CHECK(tabs[1].id == 100 && tabs[4].id == 103);
    CHECK(tabs.front().badge == 1 && tabs.back().badge == 0);
    h.Open();
    CHECK(h.state.Tab() == OverlayTab::Online && drawn == 0);
    // The keys: Ranked (its page drawn), over Tournaments and Events to
    // Leaderboard, Events, Game options, Settings, round to Online, and back.
    h.Press(ImGuiKey_E);
    CHECK(int(h.state.Tab()) == 100 && drawn > 0);
    h.Press(ImGuiKey_GamepadR1);
    CHECK(int(h.state.Tab()) == 103);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Settings);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_Q);
    CHECK(h.state.Tab() == OverlayTab::Settings);
    h.Press(ImGuiKey_GamepadL1);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_GamepadL1);
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.Press(ImGuiKey_GamepadL1);
    CHECK(int(h.state.Tab()) == 103);
    // B on a more tab's only screen closes.
    CHECK(h.Press(ImGuiKey_GamepadFaceRight).close);
    // The host's keys open by id, wherever the tab stands; never one that doesn't open.
    h.Open();
    CHECK(h.state.Switch(OverlayTab::GameOptions) && h.state.Tab() == OverlayTab::GameOptions);
    CHECK(!h.state.Switch(OverlayTab(101)) && h.state.Tab() == OverlayTab::GameOptions);
    CHECK(!h.state.Switch(OverlayTab(102)) && !h.state.Switch(OverlayTab(555)));
    h.Open(OverlayTab(101));
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Open(OverlayTab(103));
    CHECK(int(h.state.Tab()) == 103);
    // The open tab stops opening: the next that does. It goes: from the start.
    h.model.more_tabs[3].tab.available = false;
    h.Frame();
    CHECK(h.state.Tab() == OverlayTab::Events);
    h.state.Switch(OverlayTab(100));
    h.Frame();
    CHECK(int(h.state.Tab()) == 100);
    h.model.more_tabs.clear();
    h.Frame();
    CHECK(h.state.Tab() == OverlayTab::Online);

    // Nine tabs, from a narrow window to a wide one, switching on every way
    // (labels folding into icons as they need): each lands where it should.
    h.model.more_tabs = {tab(100, "Ranked"), tab(101, "Tournaments"), tab(102, "Leaderboard"), tab(103, "Stats"),
                         tab(104, "Replays")};
    h.model.more_tabs[2].tab.badge = 12;
    h.model.more_tabs[3].tab.dot = true;
    for (const float width : {640.0f, 860.0f, 1280.0f, 1920.0f})
    {
        h.Frame([width](ImGuiIO& io) { io.DisplaySize = {width, width * 0.5625f}; });
        h.Open();
        for (int n = 0; n < 9; ++n) h.Press(ImGuiKey_E);
        CHECK(h.state.Tab() == OverlayTab::Online);
        h.Press(ImGuiKey_Q);
        CHECK(h.state.Tab() == OverlayTab::Settings);
    }
}

// The red of the badges (a count's pill, a dot) in the tab strip: vertices
// of that colour above `bottom` (the strip's lower edge) in the last frame.
int BadgeVertices(float bottom)
{
    int count = 0;
    const ImDrawData* data = ImGui::GetDrawData();
    for (int n = 0; data && n < data->CmdListsCount; ++n)
        for (const ImDrawVert& vertex : data->CmdLists[n]->VtxBuffer)
        {
            const int r = int(vertex.col & 0xFF), g = int(vertex.col >> 8 & 0xFF), b = int(vertex.col >> 16 & 0xFF),
                      a = int(vertex.col >> 24 & 0xFF);
            if (vertex.pos.y < bottom && a > 200 && r > 240 && g > 70 && g < 140 && b > 85 && b < 150) ++count;
        }
    return count;
}

// A tab's badge or dot shows on the others, not while it is open (its page
// says what waits there).
void TestOpenTabBadge()
{
    constexpr float strip = 115.0f; // a 1280x720 window's tabs end at about 108
    const auto more = [](int id, const char* label, int badge, bool dot) {
        GameOverlayMoreTab tab;
        tab.tab.id = id;
        tab.tab.label = label;
        tab.tab.icon = mscharged::launcher::Icon::Trophy;
        tab.tab.badge = badge;
        tab.tab.dot = dot;
        return tab;
    };
    {
        // Online's: a challenge.
        Harness h;
        platform::ProfileMatch set;
        set.id = 5;
        set.state = "challenged";
        set.side = "challenged";
        set.stage = "invited";
        set.opponent = Online(12, "Daisy");
        set.opponent_online = true;
        h.model.online = Model(set);
        CHECK(GameOverlayTabs(h.model).front().badge == 1);
        h.Open();
        h.Frame();
        CHECK(BadgeVertices(strip) == 0);
        CHECK(h.state.Switch(OverlayTab::GameOptions));
        h.Frame();
        CHECK(BadgeVertices(strip) > 0);
        h.Press(ImGuiKey_E);
        h.Press(ImGuiKey_E);
        CHECK(h.state.Tab() == OverlayTab::Online && BadgeVertices(strip) == 0);
    }
    for (const bool dot : {false, true})
    {
        // A more tab's count, or its dot; with five tabs and with seven (icons for the others).
        for (const int tabs : {5, 7})
        {
            Harness h;
            h.model.online = Model();
            h.model.more_tabs = {more(100, "Ranked", dot ? 0 : 12, dot), more(101, "Tournaments", 0, false),
                                 more(102, "Leaderboard", 0, false)};
            if (tabs == 7)
                h.model.more_tabs.insert(h.model.more_tabs.end(),
                                         {more(103, "Stats", 0, false), more(104, "Replays", 0, false)});
            h.Open();
            h.Frame();
            CHECK(BadgeVertices(strip) > 0);
            CHECK(h.state.Switch(OverlayTab(100)));
            h.Frame();
            h.Frame();
            CHECK(BadgeVertices(strip) == 0);
            h.Press(ImGuiKey_E);
            CHECK(int(h.state.Tab()) == 101 && BadgeVertices(strip) > 0);
        }
    }
}

int main()
{
    TestScreens();
    TestGamepadSet();
    TestTabs();
    TestGameOptions();
    TestMouse();
    TestResize();
    TestSmallWindows();
    TestChallengeSetup();
    TestSearchSetup();
    TestSettings();
    TestSettingsByKeys();
    TestSignedOut();
    TestLateLists();
    TestPrompt();
    TestFrameDone();
    TestMoreTabs();
    TestOpenTabBadge();
    if (failures)
    {
        std::fprintf(stderr, "%d overlay input checks failed\n", failures);
        return 1;
    }
    std::printf("Overlay input checks passed\n");
    return 0;
}
