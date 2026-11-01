#pragma once

#include "overlay/game_overlay.h"
#include "overlay/overlay_prompt.h"

#include <SDL3/SDL.h>

#include <map>
#include <optional>
#include <string>

namespace mscharged::launcher
{
// The game's overlay (Online and Game options) with made-up players, in the
// launcher's window, for development (--page online-panel; with --screenshot
// FILE a capture). The window takes a 1280x720 game's size (at the launcher's
// scale), and the launcher's key art, blurred and darkened, stands in for the
// game's picture. RECHARGED_CAPTURE_DISCORD picks the made-up state:
// "preview" (the default) a set won with its rematch, "preview-set" a set
// being played, "preview-invite" a challenge to answer, "preview-waiting" the
// player's own challenge, "preview-ready" a set before Ready to play!,
// "preview-ended" a set lost, "preview-match" while a match is played,
// "preview-empty" nobody online, "preview-error" a message in the footer,
// "preview-player" and "preview-player-mii" a player's screen,
// "preview-options" and "preview-options-match" the Game options tab, and
// "preview-narrow" (or any state ending in "-narrow") a small window; the
// prompts "preview-prompt-won", "-lost", "-stopped", "-challenge" and
// "-rematch". The overlay's actions
// change the made-up data and go to the console; Escape closes it, F1 opens
// Online and F2 Game options (the open tab's key closes it).
class OnlinePanelPreview
{
public:
    // `renderer` makes the made-up avatars (and frees them as it goes); its
    // window takes the game's size.
    OnlinePanelPreview(SDL_Renderer* renderer, bool capture);

    // The whole window, over the art (the launcher's header layers, `art_size`
    // their size in pixels).
    void Draw(ImVec2 size, ImTextureID art_back, ImTextureID art_front, ImVec2 art_size);

private:
    void Apply(const overlay::GameOverlayActions& actions);
    void DrawPrompt();
    void DrawBackdrop(ImVec2 size, ImTextureID back, ImTextureID front, ImVec2 art_size) const;
    ImTextureID Avatar(const std::string& key) const;

    bool capture_ = false;
    std::string mode_;
    platform::PlayerLists lists_;
    std::optional<platform::ProfileMatch> match_;
    overlay::OnlinePanelContext context_;
    overlay::GameOptionsModel options_;
    overlay::GameOverlayState state_;
    overlay::OverlayPromptState prompt_state_;
    std::map<std::string, ImTextureID> avatars_;
    bool open_ = true;
    float appear_ = 0.0f;
};
} // namespace mscharged::launcher
