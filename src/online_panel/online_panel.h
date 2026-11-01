#pragma once

#include "launcher/ui_kit.h"
#include "online_panel/online_panel_model.h"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// The online panel, drawn with ImGui (and the launcher's widgets and palette)
// in whatever renderer the caller's ImGui uses: Aurora's in the game, SDL's in
// the launcher's preview (--page online-panel). A sheet at the right of the
// picture: the player, their set, then the lists, scrolling, and a hint how to
// close it. Mouse, keyboard and gamepad (ImGui's navigation) work alike.
namespace mscharged::online_panel
{
// What the player did in this frame's panel; the caller acts on it (calls
// the profile service, closes the panel). Fresh for every frame.
struct OnlinePanelActions
{
    bool close = false; // Escape or B, the close button, or a click beside the panel
    std::optional<std::int64_t> challenge;     // a player's id: a set by the player's rules
    std::optional<std::int64_t> add_friend;    // a player's id
    std::optional<std::int64_t> remove_friend; // a player's id
    std::optional<std::int32_t> add_mii_friend;    // a Mii profile
    std::optional<std::int32_t> remove_mii_friend; // a Mii profile
    std::optional<bool> set_available; // available for random games, or not
    // The set's (OnlinePanelSet::match): accept or decline its challenge,
    // Ready to play!, withdraw or cancel it, play it again.
    bool accept = false, decline = false, ready = false, cancel = false, rematch = false;

    bool Any() const
    {
        return close || challenge || add_friend || remove_friend || add_mii_friend || remove_mii_friend
            || set_available || accept || decline || ready || cancel || rematch;
    }
};

// What the panel keeps between frames; the caller holds one per panel.
struct OnlinePanelState
{
    // Set it when the panel opens: the panel then takes the keyboard and
    // gamepad (its first action, or its first row, gets the cursor).
    bool focus = true;
    bool offline_open = false; // the friends offline are shown
};

struct OnlinePanelStyle
{
    // Pixels per design unit (dp): the launcher's UI scale; in the game its
    // picture's height / 720 (a 1280x720 window draws at 1).
    float unit = 1.0f;
    // The fonts at `unit` (AddOnlinePanelFonts): caption, body, label and
    // heading are used.
    launcher::Fonts fonts;
    // The picture the panel sits over, in ImGui's display coordinates; an
    // empty area means the whole display.
    ImVec2 area_min{0.0f, 0.0f}, area_max{0.0f, 0.0f};
    // 0..1: the sheet slides in from the right (and the dim fades in) as it opens.
    float appear = 1.0f;
    // A black veil over the picture beside the sheet (0 none), on top of the blur.
    float dim = 0.25f;
    // The key the hint at the bottom names for closing ("Esc", "F1", "HOME").
    std::string close_key = "Esc";
    // The texture for a picture's key (OnlinePanelRow::image: an avatar's
    // URL, "mii:<profile>"), or 0 while there is none: its initials show then.
    // Asked every frame for each picture on screen; may start loading it.
    std::function<ImTextureID(const std::string& key)> image;
};

// Draws the panel as its own ImGui window, in the current ImGui frame.
void DrawOnlinePanel(const OnlinePanelModel& model, OnlinePanelState& state, OnlinePanelActions& actions,
                     const OnlinePanelStyle& style);

// The fonts DrawOnlinePanel uses, added to `atlas` from TrueType data (the
// launcher's Roboto) at `unit` pixels per dp, for a host whose atlas holds
// other fonts too. The atlas does not own the data: keep it while the atlas
// is built. Null fonts where the atlas refuses the data.
launcher::Fonts AddOnlinePanelFonts(ImFontAtlas& atlas, void* ttf, int ttf_size, float unit);
} // namespace mscharged::online_panel
