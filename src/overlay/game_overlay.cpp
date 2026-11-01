#include "overlay/game_overlay.h"

#include "imgui_internal.h"

namespace mscharged::overlay
{
namespace
{
// The game's own tabs, before the overlay shows them with the model's badges.
std::vector<OverlayTabs::Tab> OwnTabs()
{
    OverlayTabs::Tab online, events, options;
    online.id = int(OverlayTab::Online);
    online.label = "Online";
    online.icon = launcher::Icon::Chat;
    events.id = int(OverlayTab::Events);
    events.label = "Events";
    events.icon = launcher::Icon::Trophy;
    options.id = int(OverlayTab::GameOptions);
    options.label = "Game options";
    options.icon = launcher::Icon::Sliders;
    return {online, events, options};
}

bool OwnTab(int id)
{
    return id == int(OverlayTab::Online) || id == int(OverlayTab::Events) || id == int(OverlayTab::GameOptions);
}

// Events: where tournaments and organised match-ups will be, for now the
// promise of them.
void DrawEventsPage(const OverlayLayout& layout)
{
    using namespace launcher;
    auto* draw = ImGui::GetWindowDrawList();
    const Fonts& fonts = CurrentFonts();
    const ImVec2 center{(layout.min.x + layout.max.x) * 0.5f, (layout.min.y + layout.max.y) * 0.5f - Dp(40)};
    draw->AddCircleFilled(center, Dp(44), Col(color::text, 0.05f));
    draw->AddCircle(center, Dp(44), Col(color::border), 0, Dp(1));
    DrawIcon(draw, Icon::Trophy, center, Dp(40), Col(color::muted));
    float y = center.y + Dp(62);
    const auto line = [&](ImFont* font, ImU32 colour, const char* text, float after) {
        const ImVec2 size = TextSize(font, text);
        DrawLabel(draw, font, {center.x - size.x * 0.5f, y}, colour, text);
        y += size.y + after;
    };
    line(fonts.heading, Col(color::text), "Tournaments and match-ups", Dp(10));
    line(fonts.body, Col(color::muted), "Sign up for events, follow your bracket and play your next set from here.",
         Dp(4));
    line(fonts.body, Col(color::muted), "Results go in by themselves, as in your sets with friends.", Dp(16));
    line(fonts.caption, Col(color::dim), "COMING LATER", 0);
}

const GameOverlayMoreTab* FindMore(const GameOverlayModel& model, OverlayTab tab)
{
    for (const auto& more : model.more_tabs)
        if (more.tab.id == int(tab)) return &more;
    return nullptr;
}

// A tab's first screen.
OverlayScreens::Screen FirstScreen(OverlayTab tab, const std::string& label)
{
    if (tab == OverlayTab::Online) return OnlinePanelFirstScreen();
    if (tab == OverlayTab::GameOptions) return GameOptionsFirstScreen();
    return {0, {}, label};
}

std::string Label(const std::vector<OverlayTabs::Tab>& tabs, OverlayTab tab)
{
    for (const auto& entry : tabs)
        if (entry.id == int(tab)) return entry.label;
    return {};
}
} // namespace

std::vector<OverlayTabs::Tab> GameOverlayTabs(const GameOverlayModel& model)
{
    std::vector<OverlayTabs::Tab> tabs = OwnTabs();
    tabs[0].badge = OnlinePanelWaiting(model.online);
    for (const auto& more : model.more_tabs)
    {
        // Never over the game's own ids.
        if (OwnTab(more.tab.id)) continue;
        tabs.insert(tabs.end() - 2, more.tab);
    }
    return tabs;
}

OverlayScreens& GameOverlayState::Screens(OverlayTab which)
{
    if (which == OverlayTab::Online) return online;
    if (which == OverlayTab::GameOptions) return options;
    return more[int(which)];
}

void GameOverlayState::Open(OverlayTab which)
{
    online.Open(OnlinePanelFirstScreen());
    options.Open(GameOptionsFirstScreen());
    more.clear();
    options_state = {};
    if (tabs.empty()) tabs = OwnTabs();
    // A tab that isn't there (or doesn't open now) opens Online instead.
    tab = OverlayTabOpens(tabs, int(which)) ? which : OverlayTab::Online;
    if (which != OverlayTab::Online && which != OverlayTab::GameOptions && tab == which)
        Screens(which).Open(FirstScreen(which, Label(tabs, which)));
}

bool GameOverlayState::Switch(OverlayTab which)
{
    if (tabs.empty()) tabs = OwnTabs();
    if (!OverlayTabOpens(tabs, int(which))) return false;
    if (online.Empty() || options.Empty())
    {
        Open(which);
        return true;
    }
    if (which == tab) return true;
    // Back on this tab later, the cursor lands where it was.
    OverlayScreens& left = Screens(tab);
    left.restore = ImGui::GetCurrentContext() && OverlayHasFocus() ? GImGui->NavId : 0;
    left.focus_pending = true;
    tab = which;
    OverlayScreens& screens = Screens(tab);
    if (screens.Empty()) screens.Open(FirstScreen(which, Label(tabs, which)));
    screens.focus_pending = true;
    return true;
}

void GameOverlayState::NextTab()
{
    if (tabs.empty()) tabs = OwnTabs();
    Switch(OverlayTab(OverlayTabStep(tabs, int(tab), 1)));
}

void GameOverlayState::PreviousTab()
{
    if (tabs.empty()) tabs = OwnTabs();
    Switch(OverlayTab(OverlayTabStep(tabs, int(tab), -1)));
}

void DrawGameOverlay(const GameOverlayModel& model, GameOverlayState& state, GameOverlayActions& actions,
                     const GameOverlayStyle& style)
{
    // Ctrl+Tab switches tabs here, not ImGui's windows.
    GImGui->ConfigNavWindowingKeyNext = 0;
    GImGui->ConfigNavWindowingKeyPrev = 0;
    state.tabs = GameOverlayTabs(model);
    if (state.online.Empty() || state.options.Empty()) state.Open(state.tab);
    // The open tab went (or doesn't open any more): the next one that does.
    if (!OverlayTabOpens(state.tabs, int(state.tab)))
    {
        const OverlayTab next = OverlayTab(OverlayTabStep(state.tabs, int(state.tab), 1));
        if (!state.Switch(next)) state.tab = OverlayTab::Online;
    }

    // The tab keys, while the overlay has the keyboard and gamepad.
    if (OverlayHasFocus())
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool ctrl_tab = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab, true);
        const bool letters = !io.KeyCtrl && !io.KeyAlt && !io.KeySuper;
        const bool previous = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) || (letters && ImGui::IsKeyPressed(ImGuiKey_Q, false))
            || (ctrl_tab && io.KeyShift);
        const bool next = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)
            || ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) || (letters && ImGui::IsKeyPressed(ImGuiKey_E, false))
            || (ctrl_tab && !io.KeyShift);
        if (previous && !next) state.PreviousTab();
        else if (next && !previous) state.NextTab();
    }

    OverlayTabs tabs;
    tabs.tabs = state.tabs;
    tabs.current = int(state.tab);
    const bool online = state.tab == OverlayTab::Online, options = state.tab == OverlayTab::GameOptions;
    const bool events = state.tab == OverlayTab::Events;
    const GameOverlayMoreTab* more = online || options || events ? nullptr : FindMore(model, state.tab);
    OverlayScreens& screens = state.Screens(state.tab);
    if (screens.Empty()) screens.Open(FirstScreen(state.tab, Label(state.tabs, state.tab)));
    const std::string title = online ? OnlinePanelTitle(model.online, screens)
        : options                    ? GameOptionsTitle(screens)
        : more && !more->title.empty() ? more->title
                                       : screens.Top().title;
    const OverlayLayout layout = BeginOverlay(style, screens, title, &tabs);
    if (online) DrawOnlinePanel(model.online, screens, actions.online, style.image, layout);
    else if (options) DrawGameOptions(model.options, state.options_state, screens, actions.set_option, layout);
    else if (events) DrawEventsPage(layout);
    else if (more && more->draw) more->draw(layout);
    OverlayFooter footer;
    if (online)
    {
        footer.note = model.online.message;
        if (model.online.message_tone == Tone::Bad) footer.note_color = launcher::color::danger;
    }
    const OverlayResult result = EndOverlay(footer);
    if (result.close) actions.close = true;
    if (tabs.clicked && !result.close) state.Switch(OverlayTab(*tabs.clicked));
}
} // namespace mscharged::overlay
