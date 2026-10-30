#pragma once

// Controller menus: a gamepad player moves between a menu's buttons with the
// D-pad and the sticks instead of pointing (tweak patch 0047). The game module
// sees the remote the gamepad plays as; the host keeps what the remote has no
// room for, each gamepad player's second stick and triggers
// (src/platform/desktop_wpad.cpp), and the game module reads them as it
// navigates (src/platform/controller_menus.cpp).
extern "C" {
struct ChargedMenuPad
{
    float stick_x, stick_y; // the pointer stick after the profile's dead zones, -1 to 1, y up
    signed char page;       // -1 the left trigger, 1 the right one, 0 neither or both
};
// The gamepad player of WPAD channel `channel` (player - 1); false when no
// gamepad player plays there or the mouse points for that player. Any thread.
typedef bool (*ChargedMenuPadReader)(int channel, ChargedMenuPad* pad);
}
