#pragma once

#include "overlay/game_overlay.h"
#include "overlay/overlay_prompt.h"

#include <SDL3/SDL.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

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
// "preview-options" and "preview-options-match" the Game options tab,
// "preview-tabs" five tabs (made-up Ranked, Tournaments and Leaderboard
// between Online and Game options, empty pages: they are not in the game)
// with badges, "preview-tabs-max" seven, one of them not available, and
// "preview-narrow" (or any state ending in "-narrow") a small window; the
// prompts "preview-prompt-won", "-lost", "-stopped", "-challenge" and
// "-rematch". RECHARGED_CAPTURE_TAB opens another tab than Online (its label:
// "game-options", "ranked", ...), and RECHARGED_CAPTURE_SIZE=WxH sizes the
// window as a game's of W x H, the overlay at the game's scale for it (its
// height / 720). The overlay's actions
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
    std::vector<overlay::GameOverlayMoreTab> more_tabs_; // "preview-tabs": tabs to come
    float game_height_ = 0.0f; // RECHARGED_CAPTURE_SIZE: the game window's height
    bool open_ = true;
    float appear_ = 0.0f;
};
} // namespace mscharged::launcher
