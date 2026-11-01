// The online panel's input (src/online_panel/online_panel.cpp) in a headless
// ImGui frame loop: the cursor it puts on its first action or row when it
// opens, Enter and the gamepad's A on a row (the challenge), the heart
// right of it, Escape and B, and a click beside the sheet. No renderer: the
// frames are built and thrown away.

#include "online_panel/online_panel.h"

#include "imgui.h"

#include <cstdio>
#include <functional>

namespace platform = mscharged::platform;
using namespace mscharged::online_panel;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (ok) return;
    std::fprintf(stderr, "online_panel_draw.cpp:%d: %s\n", line, what);
    ++failures;
}
#define CHECK(x) Check((x), #x, __LINE__)

// One ImGui context of a 1280x720 window, with keyboard and gamepad navigation.
struct Harness
{
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
        // Away from the panel until a test moves it.
        io.AddMousePosEvent(-1, -1);
    }
    ~Harness() { ImGui::DestroyContext(); }

    // One frame after `input`; what the panel reported.
    OnlinePanelActions Frame(const OnlinePanelModel& model, OnlinePanelState& state,
                             const std::function<void(ImGuiIO&)>& input = {})
    {
        if (input) input(ImGui::GetIO());
        ImGui::NewFrame();
        OnlinePanelActions actions;
        OnlinePanelStyle style;
        style.unit = 1.0f;
        style.image = [](const std::string&) { return ImTextureID(0); };
        DrawOnlinePanel(model, state, actions, style);
        ImGui::Render();
        return actions;
    }

    // A key pressed and released over a few frames: what the panel reported, all together.
    OnlinePanelActions Press(const OnlinePanelModel& model, OnlinePanelState& state, ImGuiKey key)
    {
        OnlinePanelActions all;
        const auto merge = [&](const OnlinePanelActions& a) {
            all.close |= a.close;
            all.accept |= a.accept;
            all.decline |= a.decline;
            all.ready |= a.ready;
            all.cancel |= a.cancel;
            all.rematch |= a.rematch;
            if (a.challenge) all.challenge = a.challenge;
            if (a.add_friend) all.add_friend = a.add_friend;
            if (a.remove_friend) all.remove_friend = a.remove_friend;
            if (a.add_mii_friend) all.add_mii_friend = a.add_mii_friend;
            if (a.remove_mii_friend) all.remove_mii_friend = a.remove_mii_friend;
            if (a.set_available) all.set_available = a.set_available;
        };
        merge(Frame(model, state, [key](ImGuiIO& io) { io.AddKeyEvent(key, true); }));
        merge(Frame(model, state, [key](ImGuiIO& io) { io.AddKeyEvent(key, false); }));
        merge(Frame(model, state));
        return all;
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
    context.now = 1'800'000'000;
    return MakeOnlinePanelModel(lists, match, context);
}

void TestKeyboard()
{
    Harness harness;
    const auto model = Model();
    OnlinePanelState state;
    // Opening: the first row (Mario) takes the cursor; nothing happens by itself.
    CHECK(!harness.Frame(model, state).Any());
    CHECK(!state.focus);
    CHECK(!harness.Frame(model, state).Any());
    // Enter on it: the challenge.
    auto actions = harness.Press(model, state, ImGuiKey_Enter);
    CHECK(actions.challenge && *actions.challenge == 10);
    CHECK(!actions.close && !actions.remove_friend);
    // Down: Peach.
    actions = harness.Press(model, state, ImGuiKey_DownArrow);
    CHECK(!actions.Any());
    actions = harness.Press(model, state, ImGuiKey_Enter);
    CHECK(actions.challenge && *actions.challenge == 11);
    // Right: her heart, which takes her off the friends.
    harness.Press(model, state, ImGuiKey_RightArrow);
    actions = harness.Press(model, state, ImGuiKey_Enter);
    CHECK(actions.remove_friend && *actions.remove_friend == 11);
    CHECK(!actions.challenge);
    // Escape closes it.
    actions = harness.Press(model, state, ImGuiKey_Escape);
    CHECK(actions.close);
}

void TestGamepad()
{
    Harness harness;
    platform::ProfileMatch set;
    set.id = 5;
    set.state = "challenged";
    set.side = "challenged";
    set.stage = "invited";
    set.opponent = Online(11, "Peach");
    set.opponent_online = true;
    set.rules.games = 3;
    const auto model = Model(set);
    OnlinePanelState state;
    // A challenge to answer: Accept takes the cursor, A accepts.
    harness.Frame(model, state);
    harness.Frame(model, state);
    auto actions = harness.Press(model, state, ImGuiKey_GamepadFaceDown);
    CHECK(actions.accept && !actions.decline && !actions.challenge);
    // Right: Decline.
    harness.Press(model, state, ImGuiKey_GamepadDpadRight);
    actions = harness.Press(model, state, ImGuiKey_GamepadFaceDown);
    CHECK(actions.decline && !actions.accept);
    // B closes it.
    actions = harness.Press(model, state, ImGuiKey_GamepadFaceRight);
    CHECK(actions.close);
}

void TestMouse()
{
    Harness harness;
    const auto model = Model();
    OnlinePanelState state;
    state.focus = false;
    harness.Frame(model, state);
    // A click on the picture beside the sheet closes it; one on the sheet doesn't.
    harness.Frame(model, state, [](ImGuiIO& io) { io.AddMousePosEvent(900, 400); });
    auto actions = harness.Frame(model, state, [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
    actions.close |= harness.Frame(model, state, [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); }).close;
    CHECK(!actions.close);
    harness.Frame(model, state, [](ImGuiIO& io) { io.AddMousePosEvent(200, 400); });
    actions = harness.Frame(model, state, [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); });
    CHECK(actions.close);
    harness.Frame(model, state, [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); });
}
} // namespace

int main()
{
    TestKeyboard();
    TestGamepad();
    TestMouse();
    if (failures)
    {
        std::fprintf(stderr, "%d online panel input checks failed\n", failures);
        return 1;
    }
    std::printf("Online panel input checks passed\n");
    return 0;
}
