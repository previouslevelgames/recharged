#include "overlay/game_overlay.h"

#include "imgui_internal.h"

namespace mscharged::overlay
{
void GameOverlayState::Open(OverlayTab which)
{
    online.Open(OnlinePanelFirstScreen());
    options.Open(GameOptionsFirstScreen());
    options_state = {};
    tab = which;
}

void GameOverlayState::Switch(OverlayTab which)
{
    if (online.Empty() || options.Empty())
    {
        Open(which);
        return;
    }
    if (which == tab) return;
    // Back on this tab later, the cursor lands where it was.
    OverlayScreens& left = Screens(tab);
    left.restore = ImGui::GetCurrentContext() && OverlayHasFocus() ? GImGui->NavId : 0;
    left.focus_pending = true;
    tab = which;
    Screens(tab).focus_pending = true;
}

void GameOverlayState::NextTab() { Switch(OverlayTab((int(tab) + 1) % kOverlayTabs)); }

void GameOverlayState::PreviousTab() { Switch(OverlayTab((int(tab) + kOverlayTabs - 1) % kOverlayTabs)); }

void DrawGameOverlay(const GameOverlayModel& model, GameOverlayState& state, GameOverlayActions& actions,
                     const GameOverlayStyle& style)
{
    // Ctrl+Tab switches tabs here, not ImGui's windows.
    GImGui->ConfigNavWindowingKeyNext = 0;
    GImGui->ConfigNavWindowingKeyPrev = 0;
    if (state.online.Empty() || state.options.Empty()) state.Open(state.tab);

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
    tabs.tabs = {{"Online", launcher::Icon::Chat}, {"Game options", launcher::Icon::Sliders}};
    tabs.current = int(state.tab);
    const bool online = state.tab == OverlayTab::Online;
    OverlayScreens& screens = state.Screens(state.tab);
    const std::string title = online ? OnlinePanelTitle(model.online, screens) : GameOptionsTitle(screens);
    const OverlayLayout layout = BeginOverlay(style, screens, title, &tabs);
    if (online) DrawOnlinePanel(model.online, screens, actions.online, style.image, layout);
    else DrawGameOptions(model.options, state.options_state, screens, actions.set_option, layout);
    OverlayFooter footer;
    if (online)
    {
        footer.note = model.online.message;
        if (model.online.message_tone == Tone::Bad) footer.note_color = launcher::color::danger;
    }
    const OverlayResult result = EndOverlay(footer);
    if (result.close) actions.close = true;
    if (tabs.clicked >= 0 && !result.close) state.Switch(OverlayTab(tabs.clicked));
}
} // namespace mscharged::overlay
