#pragma once

// The game's menus Recharged stands in for (the development switch "Override
// retail menus", tweak patch 0070), as the game module hands them to the host.
//
// The game's own scene runs as always, only undrawn: the match's pause menu
// (its players point, hover, press and resume in it, with its sounds) and the
// front end's Options (whose camera, slides and way back stay the game's). The
// host draws Recharged's version of them from this snapshot, which the module
// fills after a game frame (charged_menu_snapshot) on the thread that runs
// the game's tasks; reading changes nothing in the game. What the host wants
// the game to do (resume, a sound, the game's own options) goes through
// charged_menu_command, done in the game's next frame.
//
// Positions are the front end's units, as the game's pointers and buttons use
// them: (0, 0) in the middle of the picture, y up, fe_height units from the
// bottom of the picture to its top.
struct ChargedMenuSnapshot
{
    // The game's menu on top now that Recharged stands in for (ChargedMenu);
    // CHARGED_MENU_NONE for none (or the switch off).
    int menu;
    // Its scene is set up and takes the players' input (the pause menu's
    // buttons are placed, the Options' slide has come in).
    int ready;
    // It is on its way out (resuming, quitting, to the Credits or back to the
    // main menu): the host's menu goes too.
    int leaving;
    // The front end's picture's height in its units (the screen's).
    float fe_height;
    // The pause menu's buttons by ePauseMenuAction (0 Resume, 1 Choose Sides,
    // 2 Audio Options, 3 Visual Options, 4 Statistics, 5 Quit, 6 Controller
    // Map): where a pointer selects them, whether one hovers over them, and
    // their texts as the game would show them, in its language (UTF-8, empty
    // when it has none).
    struct Button
    {
        float min_x, max_x, min_y, max_y;
        int hovered;
        char label[64];
    } buttons[7];
    // The menu's title ("PAUSED"), UTF-8; empty when the scene has none.
    char title[64];
    // The game's header picture behind the title (the orange bar with the
    // game's logo, cupscreenheader_default): its texture (0 for none) and the
    // rectangle the game gives it, centre and size in front end units.
    unsigned header_texture;
    float header_x, header_y, header_width, header_height;
    // Each player's pointer where the front end has it.
    float pointer_x[4], pointer_y[4];
    // The player whose pointer the front end's menus follow (the others wait),
    // -1 in a match (every player's).
    int controller;
    // The players' cursors as the game drew them this frame (its own
    // pictures and numbers, wiicursor.fen), in drawing order: each a textured
    // quad of a player's cursor (pad 0-3), corners in front end units (top
    // left, bottom left, bottom right, top right), its texture coordinates and
    // colour. The game leaves them out of its picture while Recharged's menu
    // shows; the host draws them on top.
    struct Quad
    {
        unsigned texture; // charged_menu_texture's key
        int pad;
        float x[4], y[4], u[4], v[4];
        unsigned char rgba[4];
    } quads[64];
    int quad_count;
    // The game's buttons Recharged's pages take instead of the game's own
    // pages (the pause menu's Audio and Visual Options, the main menu's
    // ONLINE): how many presses so far (it only grows), and the last one's
    // menu (ChargedMenu), button and player.
    unsigned taken;
    int taken_menu, taken_button, taken_pad;
    // The game's own options, as its save holds them: its volume bars (0-10)
    // and the match camera (auto zoom, the zoom level 0-4).
    int music_volume, effects_volume, voice_volume;
    int auto_zoom, zoom_level;
    // The front end (the game's menus), not a match.
    int front_end;
    // A match against another machine is running (ChargedNetworkMatch).
    // Nothing of Recharged's menus may then change the game (its sounds draw
    // the random numbers both machines share): charged_menu_command takes
    // only CHARGED_MENU_CURSORS and CHARGED_MENU_FORFEIT then.
    int network_match;
    // The player forfeited it (CHARGED_MENU_FORFEIT): on the way out.
    int forfeiting;
};

enum ChargedMenu
{
    CHARGED_MENU_NONE = 0,
    CHARGED_MENU_PAUSE = 1,   // the match's pause menu (pausemenu_v3.fen; not Strikers 101's)
    CHARGED_MENU_OPTIONS = 2, // the front end's Options (options_main_menu.fen)
    CHARGED_MENU_MAIN = 3,    // the main menu (its wheel stays the game's; only a taken press names it)
};

enum ChargedNetworkMatch
{
    CHARGED_NETWORK_NONE = 0,
    CHARGED_NETWORK_LAN = 1,
    CHARGED_NETWORK_ONLINE = 2,
};

// The cursor's A (over a button: the hand's texture key), which the hand at
// the mouse shows only over an item.
#define CHARGED_MENU_CURSOR_A_TEXTURE 0x97BF18BCu

// The pause menu's buttons from top to bottom, as the game's scene places
// them (Controller Map above Quit), by ePauseMenuAction.
inline constexpr int kChargedPauseMenuOrder[7] = {0, 1, 2, 3, 4, 6, 5};

// What the host asks the game's menus to do, in the game's next frame.
enum ChargedMenuCommandKind
{
    CHARGED_MENU_SOUND = 1,     // the front end's sound `which` (ChargedMenuSound)
    CHARGED_MENU_VOICE = 2,     // a voice at the voice volume, as the game's Audio Options plays one
    CHARGED_MENU_VOLUME = 3,    // the game's volume bar `which` (0 music, 1 effects, 2 voices) to `value` (0-10)
    CHARGED_MENU_AUTO_ZOOM = 4, // the match camera's auto zoom on (`value` 1) or off
    CHARGED_MENU_ZOOM = 5,      // the match camera's zoom level (`value` 0-4)
    CHARGED_MENU_SAVE = 6,      // the game saves its options (as its SAVE button)
    CHARGED_MENU_RESUME = 7,    // the pause menu resumes the match (as its Resume)
    CHARGED_MENU_PRESS = 8,     // the Options' button `which` is pressed (2: Credits)
    CHARGED_MENU_BACK = 9,      // the Options go back to the main menu (as its BACK)
    CHARGED_MENU_WII_ONLINE = 10, // the main menu's ONLINE the game's way (Mii, login, its online menu)
    CHARGED_MENU_CURSORS = 11,  // the game hands its players' cursors over (`value` 1) or draws them (0)
    CHARGED_MENU_FORFEIT = 12,  // the online match is left, as the game leaves it when the disc lid opens
};

// The game's own menu sounds (CHARGED_MENU_SOUND): a button lit, pressed,
// a page left, a bar's step (its Audio Options), a switch (its camera's zoom
// mode).
enum ChargedMenuSound
{
    CHARGED_SOUND_HOVER = 0,
    CHARGED_SOUND_SELECT = 1,
    CHARGED_SOUND_BACK = 2,
    CHARGED_SOUND_STEP = 3,
    CHARGED_SOUND_SWITCH = 4,
};

struct ChargedMenuCommand
{
    int kind; // ChargedMenuCommandKind
    int which;
    int value;
};

#ifdef __cplusplus
extern "C" {
#endif
// Game module entry: fills `snapshot` when `size` matches this build's
// struct; false otherwise, and with the switch off.
typedef bool (*ChargedMenuSnapshotFunction)(struct ChargedMenuSnapshot* snapshot, unsigned size);
// Game module entry: the picture of a quad's texture as 8-bit RGBA rows, top
// to bottom: its size in *width and *height, its GX format (the game's
// eGXTextureFormat) in *format, and with `rgba` and enough `capacity`
// (width * height * 4 bytes) its texels. False for a texture the game does not
// have or a format it cannot decode.
typedef bool (*ChargedMenuTextureFunction)(unsigned texture, int* width, int* height, int* format,
                                           unsigned char* rgba, unsigned capacity);
// Game module entry: queues `command` for the game's next frame; false when
// `size` does not match or the queue is full.
typedef bool (*ChargedMenuCommandFunction)(const struct ChargedMenuCommand* command, unsigned size);
#ifdef __cplusplus
}

// Game module (competitive_match.cpp's popup hook): whether Recharged's menus
// answer the game's popup `popup_type` themselves (ePopupMenu: the network
// popup after the player forfeited, its only button).
bool ChargedMenuAnswersPopup(int popup_type);
#endif
