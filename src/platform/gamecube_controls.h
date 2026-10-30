#pragma once

// GameCube controls: a GameCube controller whose gamepad profile plays with
// them is the engine's GameCube controller in matches, with Super Mario
// Strikers' controls (tweak patch 0046). The host keeps each such player's
// controller state (src/platform/desktop_wpad.cpp) and the game module reads
// it as the match takes the player's input (src/platform/game_tweaks.cpp).
extern "C" {
struct ChargedGameCubePad
{
    unsigned short buttons;                   // PAD_BUTTON_* and PAD_TRIGGER_* bits, as PADStatus has them
    unsigned char trigger_left, trigger_right; // analog L and R, 0-255
    float substick_x, substick_y;              // the C-stick after the profile's dead zones, -1 to 1, y up
};
// The controller of WPAD channel `channel` (player - 1); false when that
// player does not play with GameCube controls. Any thread.
typedef bool (*ChargedGameCubePadReader)(int channel, ChargedGameCubePad* pad);
}
