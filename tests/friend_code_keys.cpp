#include "platform/friend_code_keys.h"

#include <iostream>
#include <stdexcept>
#include <string>

// The number keys must press the friend code keypad's buttons: 0-8 are the
// digits 1-9 and 9 is 0 (SHOnlineFriendCodeEntry::OnKeypadPointerPress).
namespace {
unsigned checks = 0;
void Check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) throw std::runtime_error("Friend code key changed: " + what);
}
}

int main()
{
    using mscharged::platform::FriendCodeKeypadItem;
    try
    {
        Check(FriendCodeKeypadItem(SDL_SCANCODE_1) == 0, "1 presses the first button");
        Check(FriendCodeKeypadItem(SDL_SCANCODE_9) == 8, "9 presses the ninth button");
        Check(FriendCodeKeypadItem(SDL_SCANCODE_0) == 9, "0 presses the button after 9");
        Check(FriendCodeKeypadItem(SDL_SCANCODE_KP_1) == 0, "keypad 1 is 1");
        Check(FriendCodeKeypadItem(SDL_SCANCODE_KP_0) == 9, "keypad 0 is 0");
        // Back and A, and the keypad's OK and backspace, are not typed.
        for (SDL_Scancode key : {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER,
                                 SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_A, SDL_SCANCODE_MINUS})
            Check(FriendCodeKeypadItem(key) == -1, "key " + std::to_string(key) + " types nothing");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Friend code keys: " << checks << " checks passed.\n";
    return 0;
}
