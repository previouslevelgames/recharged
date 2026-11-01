#pragma once

#include "online_panel/online_panel.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace mscharged::launcher
{
// The game's online panel with made-up players, in the launcher's window, for
// development (--page online-panel; with --screenshot FILE a capture). The
// launcher's key art, blurred and darkened, stands in for the game's picture.
// RECHARGED_CAPTURE_DISCORD picks the made-up state: "preview" (the default)
// a set being played, "preview-invite" a challenge to answer,
// "preview-waiting" the player's own challenge, "preview-ready" a set before
// Ready to play!, "preview-ended" a set won, "preview-offline" the friends
// offline shown and no set, "preview-match" while a match is played,
// "preview-empty" nobody at all. The panel's actions change the made-up
// players and go to the console; Escape closes it and F1 opens it again.
class OnlinePanelPreview
{
public:
    // `renderer` makes the made-up avatars (and frees them as it goes).
    OnlinePanelPreview(SDL_Renderer* renderer, bool capture);

    // The whole window, over the art (the launcher's header layers, `art_size`
    // their size in pixels).
    void Draw(ImVec2 size, ImTextureID art_back, ImTextureID art_front, ImVec2 art_size);

private:
    void Apply(const online_panel::OnlinePanelActions& actions);
    void DrawBackdrop(ImVec2 size, ImTextureID back, ImTextureID front, ImVec2 art_size) const;
    ImTextureID Avatar(const std::string& key) const;

    bool capture_ = false;
    std::string mode_;
    platform::PlayerLists lists_;
    std::optional<platform::ProfileMatch> match_;
    online_panel::OnlinePanelContext context_;
    online_panel::OnlinePanelState state_;
    std::map<std::string, ImTextureID> avatars_;
    bool open_ = true;
    float appear_ = 0.0f;
};
} // namespace mscharged::launcher
