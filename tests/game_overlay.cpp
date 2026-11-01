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

#include "imgui.h"

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
    if (a.online.add_friend) o.add_friend = a.online.add_friend;
    if (a.online.remove_friend) o.remove_friend = a.online.remove_friend;
    if (a.online.add_mii_friend) o.add_mii_friend = a.online.add_mii_friend;
    if (a.online.remove_mii_friend) o.remove_mii_friend = a.online.remove_mii_friend;
    if (a.online.set_available) o.set_available = a.online.set_available;
    if (a.set_option) all.set_option = a.set_option;
}

// One ImGui context of a 1280x720 window, with keyboard and gamepad navigation.
struct Harness
{
    GameOverlayModel model;
    GameOverlayState state;

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
    return MakeOnlinePanelModel(lists, match, context);
}

void TestScreens()
{
    Harness h;
    h.model.online = Model();
    h.Open();
    CHECK(h.state.Tab() == OverlayTab::Online && h.OnMain());
    // A row opens its player; their screen's cursor is on Challenge, which
    // challenges and goes back to the list.
    auto actions = h.Press(ImGuiKey_Enter);
    CHECK(!actions.Any());
    CHECK(h.OnPlayer("p10"));
    actions = h.Press(ImGuiKey_Enter);
    CHECK(actions.online.challenge && *actions.online.challenge == 10);
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
    // Every way to the other tab and back.
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_Q);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_GamepadR1);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_GamepadL1);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_GamepadR2);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_GamepadL2);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_Tab, true);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_Tab, true, true);
    CHECK(h.state.Tab() == OverlayTab::Online);
    // Two tabs: next from the last is the first.
    h.Press(ImGuiKey_E);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Online);
    // A click on Game options (right of the middle of the tabs at the top).
    auto actions = h.Click(700, 86);
    CHECK(!actions.close && h.state.Tab() == OverlayTab::GameOptions);
    h.Click(570, 86);
    CHECK(h.state.Tab() == OverlayTab::Online);

    // Each tab keeps its screens while the overlay is open.
    h.Press(ImGuiKey_Enter);
    CHECK(h.OnPlayer("p10"));
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
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
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.state.PreviousTab();
    CHECK(h.state.Tab() == OverlayTab::Online);
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
    // Online first, Game options last, the more tabs between; Online's badge: the challenge.
    const auto tabs = GameOverlayTabs(h.model);
    CHECK(tabs.size() == 6 && tabs.front().id == int(OverlayTab::Online));
    CHECK(tabs.back().id == int(OverlayTab::GameOptions) && tabs[1].id == 100 && tabs[4].id == 103);
    CHECK(tabs.front().badge == 1 && tabs.back().badge == 0);
    h.Open();
    CHECK(h.state.Tab() == OverlayTab::Online && drawn == 0);
    // The keys: Ranked (its page drawn), over Tournaments and Events to
    // Leaderboard, Game options, round to Online, and back.
    h.Press(ImGuiKey_E);
    CHECK(int(h.state.Tab()) == 100 && drawn > 0);
    h.Press(ImGuiKey_GamepadR1);
    CHECK(int(h.state.Tab()) == 103);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.Press(ImGuiKey_E);
    CHECK(h.state.Tab() == OverlayTab::Online);
    h.Press(ImGuiKey_Q);
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
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
    CHECK(h.state.Tab() == OverlayTab::GameOptions);
    h.state.Switch(OverlayTab(100));
    h.Frame();
    CHECK(int(h.state.Tab()) == 100);
    h.model.more_tabs.clear();
    h.Frame();
    CHECK(h.state.Tab() == OverlayTab::Online);

    // Seven tabs, from a narrow window to a wide one, switching on every way
    // (labels folding into icons as they need): each lands where it should.
    h.model.more_tabs = {tab(100, "Ranked"), tab(101, "Tournaments"), tab(102, "Leaderboard"), tab(103, "Stats"),
                         tab(104, "Replays")};
    h.model.more_tabs[2].tab.badge = 12;
    h.model.more_tabs[3].tab.dot = true;
    for (const float width : {640.0f, 860.0f, 1280.0f, 1920.0f})
    {
        h.Frame([width](ImGuiIO& io) { io.DisplaySize = {width, width * 0.5625f}; });
        h.Open();
        for (int n = 0; n < 7; ++n) h.Press(ImGuiKey_E);
        CHECK(h.state.Tab() == OverlayTab::Online);
        h.Press(ImGuiKey_Q);
        CHECK(h.state.Tab() == OverlayTab::GameOptions);
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
    TestLateLists();
    TestPrompt();
    TestFrameDone();
    TestMoreTabs();
    if (failures)
    {
        std::fprintf(stderr, "%d overlay input checks failed\n", failures);
        return 1;
    }
    std::printf("Overlay input checks passed\n");
    return 0;
}
