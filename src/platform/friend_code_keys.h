#pragma once
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <optional>
#include <string_view>

namespace mscharged::platform
{
// The keypad button of the online friend code entry (SHOnlineFriendCodeEntry)
// that a number key presses: 0-8 for the digits 1-9, 9 for 0, from the top row
// or the keypad; -1 for any other key. Enter stays the menu's A.
int FriendCodeKeypadItem(SDL_Scancode key);

// The keypad's own delete button (OnKeypadPointerPress item 11): the selected
// box, or the one before it when that is empty.
inline constexpr int kFriendCodeDeleteItem = 11;
// Not keypad buttons: the entry screen empties the code, or plays the
// keypad's error sound for a paste that is no friend code.
inline constexpr int kFriendCodeClearItem = 12;
inline constexpr int kFriendCodeRefusedItem = 13;

// What a key does on the entry screen besides typing a digit: Backspace
// deletes the last digit, Delete empties the code, Ctrl+V or Cmd+V (without
// Alt) or Shift+Insert pastes one. The screen takes these keys from the game
// while it asks for keys, so Backspace is not the menu's Back there (Escape
// still is).
enum class FriendCodeEdit
{
    None,
    Delete,
    Clear,
    Paste,
};
FriendCodeEdit FriendCodeEditFor(SDL_Scancode key, SDL_Keymod mods);

// A friend code in pasted text: its 12 digits, with any spaces, dashes or
// dots around and between them ("0310-6477-1073", "086899 345920"); nothing
// when the text holds anything else or another number of digits.
std::optional<std::array<int, 12>> ParseFriendCode(std::string_view text);
}

// For the game module (patch 0756): the keypad button of the next key pressed
// on the entry screen (the constants above), oldest first, or -1. A paste
// comes as the clear item and then the code's twelve digits; a paste that is
// no code as the refused item. The entry screen asks every frame; keys
// pressed while it has not asked for a moment are dropped, so typing
// elsewhere never reaches a later code.
extern "C" int MSChargedTakeFriendCodeKey(void);
