// The game's overlay (src/overlay) in a headless ImGui frame loop: where the
// keyboard and gamepad cursor lands as a screen or tab opens and as one comes
// back, A and Enter on a row (its player's screen) and on its buttons, B and
// Backspace (back, or close on a first screen), Escape, the tab keys (L1/R1,
// L2/R2, Q/E, Ctrl+Tab), a click on a tab or beside the window, each tab's own
// screens, Game options' switches and the frame's Done. No renderer: the
// frames are built and thrown away.

#include "overlay/game_overlay.h"

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

int main()
{
    TestScreens();
    TestGamepadSet();
    TestTabs();
    TestGameOptions();
    TestMouse();
    TestResize();
    TestFrameDone();
    if (failures)
    {
        std::fprintf(stderr, "%d overlay input checks failed\n", failures);
        return 1;
    }
    std::printf("Overlay input checks passed\n");
    return 0;
}
