#include "platform/friend_code_keys.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The number keys must press the friend code keypad's buttons: 0-8 are the
// digits 1-9 and 9 is 0 (SHOnlineFriendCodeEntry::OnKeypadPointerPress); 11 is
// its delete button. Backspace, Delete and a paste act on the code while the
// entry screen asks for keys, and stay out of the game.
namespace {
unsigned checks = 0;
void Check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) throw std::runtime_error("Friend code key changed: " + what);
}

using namespace mscharged::platform;

void TestKeys()
{
    Check(FriendCodeKeypadItem(SDL_SCANCODE_1) == 0, "1 presses the first button");
    Check(FriendCodeKeypadItem(SDL_SCANCODE_9) == 8, "9 presses the ninth button");
    Check(FriendCodeKeypadItem(SDL_SCANCODE_0) == 9, "0 presses the button after 9");
    Check(FriendCodeKeypadItem(SDL_SCANCODE_KP_1) == 0, "keypad 1 is 1");
    Check(FriendCodeKeypadItem(SDL_SCANCODE_KP_0) == 9, "keypad 0 is 0");
    // A, and the keypad's OK and backspace, are not typed.
    for (SDL_Scancode key : {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER,
                             SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_A, SDL_SCANCODE_MINUS})
        Check(FriendCodeKeypadItem(key) == -1, "key " + std::to_string(key) + " types nothing");

    Check(FriendCodeEditFor(SDL_SCANCODE_BACKSPACE, SDL_KMOD_NONE) == FriendCodeEdit::Delete, "Backspace deletes");
    Check(FriendCodeEditFor(SDL_SCANCODE_KP_BACKSPACE, SDL_KMOD_NONE) == FriendCodeEdit::Delete,
          "the keypad's backspace deletes");
    Check(FriendCodeEditFor(SDL_SCANCODE_DELETE, SDL_KMOD_NONE) == FriendCodeEdit::Clear, "Delete empties the code");
    Check(FriendCodeEditFor(SDL_SCANCODE_V, SDL_KMOD_LCTRL) == FriendCodeEdit::Paste, "Ctrl+V pastes");
    Check(FriendCodeEditFor(SDL_SCANCODE_V, SDL_KMOD_RGUI) == FriendCodeEdit::Paste, "Cmd+V pastes");
    Check(FriendCodeEditFor(SDL_SCANCODE_INSERT, SDL_KMOD_LSHIFT) == FriendCodeEdit::Paste, "Shift+Insert pastes");
    Check(FriendCodeEditFor(SDL_SCANCODE_V, SDL_KMOD_NONE) == FriendCodeEdit::None, "V alone is the game's");
    Check(FriendCodeEditFor(SDL_SCANCODE_V, SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LALT)) == FriendCodeEdit::None,
          "Ctrl+Alt+V is no paste");
    Check(FriendCodeEditFor(SDL_SCANCODE_INSERT, SDL_KMOD_NONE) == FriendCodeEdit::None, "Insert alone is the game's");
    Check(FriendCodeEditFor(SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE) == FriendCodeEdit::None, "Escape stays Back");
    Check(FriendCodeEditFor(SDL_SCANCODE_RETURN, SDL_KMOD_NONE) == FriendCodeEdit::None, "Enter stays A");
}

void TestCodes()
{
    const std::array<int, 12> code{0, 8, 6, 8, 9, 9, 3, 4, 5, 9, 2, 0};
    for (const char* text : {"086899345920", "086899 345920", "0868-9934-5920", " 0868 9934 5920\n", "0868.9934.5920",
                             "0868\xE2\x80\x93" "9934\xE2\x80\x93" "5920", "086899\xC2\xA0" "345920",
                             "086899\xE2\x80\xAF" "345920", "\t0868-9934-5920\r\n"})
    {
        const auto parsed = ParseFriendCode(text);
        Check(parsed && *parsed == code, std::string("a code: ") + text);
    }
    for (const char* text : {"", "08689934592", "0868993459201", "0868a9934592", "0868_9934_5920", "+086899345920",
                             "FC 086899345920", "0868 9934 5920 1", "\xEF\xBC\x90" "86899345920"})
        Check(!ParseFriendCode(text), std::string("no code: ") + text);
}

// Through SDL as the game sees it: the filter and the watch, the queue the
// entry screen takes, and what reaches the game's own event handling.
std::vector<SDL_Scancode> QueuedKeyDowns()
{
    std::vector<SDL_Scancode> keys;
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_EVENT_KEY_DOWN) keys.push_back(event.key.scancode);
    return keys;
}

void Press(SDL_Scancode key, SDL_Keymod mods = SDL_KMOD_NONE, bool down = true, bool repeat = false)
{
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = key;
    event.key.mod = mods;
    event.key.down = down;
    event.key.repeat = repeat;
    SDL_PushEvent(&event);
}

std::vector<int> Taken()
{
    std::vector<int> items;
    for (int item = MSChargedTakeFriendCodeKey(); item >= 0; item = MSChargedTakeFriendCodeKey()) items.push_back(item);
    return items;
}

void TestScreen()
{
    // The screen asks: the filter is in place.
    Check(MSChargedTakeFriendCodeKey() == -1, "nothing pressed yet");
    QueuedKeyDowns();

    Press(SDL_SCANCODE_4);
    Press(SDL_SCANCODE_BACKSPACE);
    Press(SDL_SCANCODE_BACKSPACE, SDL_KMOD_NONE, true, true);
    Press(SDL_SCANCODE_BACKSPACE, SDL_KMOD_NONE, false);
    Press(SDL_SCANCODE_DELETE);
    Press(SDL_SCANCODE_DELETE, SDL_KMOD_NONE, true, true);
    Press(SDL_SCANCODE_DELETE, SDL_KMOD_NONE, false);
    const auto items = Taken();
    std::string got;
    for (const int item : items) got += " " + std::to_string(item);
    Check(items == std::vector<int>{3, kFriendCodeDeleteItem, kFriendCodeDeleteItem, kFriendCodeClearItem},
          "a digit, two deletes (one held), a clear; got" + got);
    const auto reached = QueuedKeyDowns();
    Check(reached == std::vector<SDL_Scancode>{SDL_SCANCODE_4}, "only the digit reaches the game");

    if (SDL_SetClipboardText("0310-6477-1073") && SDL_HasClipboardText())
    {
        Press(SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL);
        Press(SDL_SCANCODE_V, SDL_KMOD_LCTRL);
        Press(SDL_SCANCODE_V, SDL_KMOD_LCTRL, false);
        Check(Taken() == std::vector<int>{kFriendCodeClearItem, 9, 2, 0, 9, 5, 3, 6, 6, 0, 9, 6, 2},
              "a pasted code: clear, then its digits as keypad buttons");
        Check(QueuedKeyDowns() == std::vector<SDL_Scancode>{SDL_SCANCODE_LCTRL}, "Ctrl reaches the game, V does not");

        SDL_SetClipboardText("0310-6477-107");
        Press(SDL_SCANCODE_INSERT, SDL_KMOD_LSHIFT);
        Check(Taken() == std::vector<int>{kFriendCodeRefusedItem}, "eleven digits: refused");
        SDL_SetClipboardText("hello 0310-6477-1073");
        Press(SDL_SCANCODE_V, SDL_KMOD_LGUI);
        Check(Taken() == std::vector<int>{kFriendCodeRefusedItem}, "other text: refused");
        QueuedKeyDowns();
    }
    else
        std::cout << "Friend code keys: no clipboard on this video driver, paste through SDL not checked.\n";

    // Once the screen stops asking, the keys are the game's again.
    SDL_Delay(300);
    Press(SDL_SCANCODE_BACKSPACE);
    Press(SDL_SCANCODE_BACKSPACE, SDL_KMOD_NONE, false);
    Check(QueuedKeyDowns() == std::vector<SDL_Scancode>{SDL_SCANCODE_BACKSPACE}, "Backspace is Back away from the screen");
    Check(MSChargedTakeFriendCodeKey() == -1, "nothing left over for the next visit");
}
}

int main()
{
    try
    {
        TestKeys();
        TestCodes();
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(std::string("SDL: ") + SDL_GetError());
        TestScreen();
        SDL_Quit();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Friend code keys: " << checks << " checks passed.\n";
    return 0;
}
