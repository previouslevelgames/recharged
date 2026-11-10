#pragma once

#include "overlay/game_overlay.h"
#include "overlay/overlay_prompt.h"
#include "ui/rui_kit.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Recharged's pages in the UI kit's TV (rui_kit.h): the Recharged menu (HOME,
// F1); the OPTIONS hub (the main menu's OPTIONS, the Recharged menu's) with
// GAMEPLAY and SETTINGS (SETTINGS' SOUND and DISPLAY are what the pause menu's
// Audio and Visual Options open); the ONLINE hub (the main menu's globe, the
// Recharged menu's) with the player's set, PLAY (FRIENDS, RANDOM OPPONENT,
// RANKED; a player's page and a challenge's setup), PROFILE (records and
// friends) and LEADERBOARD; and the prompt over everything (a challenge, a
// set's end). They draw from a
// model the host fills each frame (the overlay's: overlay/game_overlay.h) and
// say what the player did; the host applies it.
namespace mscharged::ui
{
enum class PageId
{
    Menu,        // the Recharged menu: ONLINE, OPTIONS, QUIT RECHARGED
    Options,     // the hub: GAMEPLAY, SETTINGS (and the game's CREDITS in the menus)
    Gameplay,    // Recharged's gameplay options, by category
    Settings,    // the display (the game's match camera first), the sound, the rest
    Quit,        // QUIT RECHARGED: are you sure?
    Online,      // the hub: the player's set, PLAY, PROFILE, LEADERBOARD (and WII ONLINE from the main menu)
    Play,        // FRIENDS, RANDOM OPPONENT, RANKED
    Friends,     // the friends online, to challenge
    Random,      // open to challenges, the search for an opponent, the others open to challenges
    Profile,     // the player, their records and all their friends
    Player,      // one player (the page's key): challenge, friend
    Setup,       // a challenge's rules (key: the player's), or a search's ("search")
    Ranked,      // Play RANKED, the player's place this season and today
    Leaderboard, // the season's and today's leaders
    Prompt,      // a question over everything: a challenge, a set's end
    Match,       // an online match's menu (the game's pause; nothing pauses): CONTINUE, FORFEIT
    Forfeit,     // forfeit the online match? (then: on the way out)
};

// The categories of Settings (also where the pause menu's Audio and Visual
// Options open it).
enum SettingsCategory
{
    kSettingsDisplay = 0,
    kSettingsSound = 1,
    kSettingsAdvanced = 2,
};

// The search setup's key (PageId::Setup).
inline constexpr const char* kSearchKey = overlay::kOnlinePanelSearchKey;

// The game's own options, as its save holds them.
struct GameValues
{
    bool known = false;                         // read from the game (else not shown)
    int music = 10, effects = 10, voices = 10;  // its volume bars, 0-10
    bool auto_zoom = true;
    int zoom = 0; // the camera's zoom level, 0-4
};

struct PageModel
{
    // The settings, the gameplay options, Online and Ranked (the overlay's).
    overlay::GameOverlayModel overlay;
    GameValues game;
    bool front_end = false;  // the game's menus (the Credits are there), not a match
    bool from_wheel = false; // the ONLINE hub opened from the main menu's globe: WII ONLINE goes on there
    // A match against another machine runs: the game's own options stay as
    // they are (changing them would change the game on one machine only).
    bool network_match = false;
    bool forfeiting = false; // the player forfeited it: on the way out
    bool set_match = false;  // the online match is a set's (a forfeit gives the set up)
    // A picture's texture (a player's Mii: "mii:own", "miidata:<record>"), 0
    // for none; the host's, for this frame.
    const std::function<ImTextureID(const std::string&)>* image = nullptr;
    // The question the prompt page asks (the host's, for this frame).
    const overlay::OverlayPrompt* prompt = nullptr;
};

// What the pages keep between frames besides their lists: the challenge
// being set up.
struct PageState
{
    overlay::OnlinePanelState online;
};

// What the player did on a page this frame.
struct PageActions
{
    std::optional<overlay::GameSettingChange> setting;
    std::optional<overlay::GameOptionChange> option;
    std::optional<int> music, effects, voices; // the game's bars, 0-10
    std::optional<bool> auto_zoom;
    std::optional<int> zoom;
    std::optional<PageId> open; // a page over this one
    std::string open_key;       // its key (a player's)
    int open_category = 0;      // its category to show first
    bool credits = false;       // the game's Credits (the front end's Options)
    bool wii_online = false;    // the game's own online, as its main menu's globe
    overlay::OnlinePanelActions online;
    overlay::RankedActions ranked;
    int answer = 0;    // the prompt: 1 its first button, 2 its second
    bool forfeit = false; // the online match is left
    bool close = false; // the Recharged menu closes
    bool back = false;  // back a page (CANCEL)
    bool quit = false;  // the game ends (QUIT RECHARGED)
};

// A page's title in its header.
std::string PageTitle(PageId id, const std::string& key, const PageModel& model);
// Its categories (in a column at its left), none for a page without; in the
// player's language.
std::vector<std::string> PageCategories(PageId id);
// Whether it is shown in the wide TV (those with categories, and the long ones).
bool PageWide(PageId id);
// Whether its TV is as short as its items (a question, the match menu), with
// no footer: its answers are on its screen, B is the way back.
bool PageCompact(PageId id);
// Its items in `list`: those of its category `category`.
void DrawPage(PageId id, int category, const std::string& key, const PageModel& model, PageState& state, List& list,
              PageActions& actions);
} // namespace mscharged::ui
