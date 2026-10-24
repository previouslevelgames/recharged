#pragma once
#include <SDL3/SDL_scancode.h>

namespace mscharged::platform
{
// The keypad button of the online friend code entry (SHOnlineFriendCodeEntry)
// that a number key presses: 0-8 for the digits 1-9, 9 for 0, from the top row
// or the keypad; -1 for any other key. Backspace and Enter stay the menu's
// Back and A.
int FriendCodeKeypadItem(SDL_Scancode key);
}

// For the game module (patch 0756): the keypad button of the next number key
// pressed, oldest first, or -1. The entry screen asks every frame; keys pressed
// while it has not asked for a moment are dropped, so typing elsewhere never
// reaches a later code.
extern "C" int MSChargedTakeFriendCodeKey(void);
