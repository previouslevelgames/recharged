#include "platform/friend_code_keys.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

namespace mscharged::platform
{
int FriendCodeKeypadItem(SDL_Scancode key)
{
    // SDL orders both rows 1-9, then 0, as the keypad buttons are.
    if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_0) return key - SDL_SCANCODE_1;
    if (key >= SDL_SCANCODE_KP_1 && key <= SDL_SCANCODE_KP_0) return key - SDL_SCANCODE_KP_1;
    return -1;
}
}

namespace
{
// A friend code has 12 digits; the screen asks about every 16 ms.
constexpr std::size_t kMaxKeys = 12;
constexpr Uint64 kAskingMs = 250;

std::mutex sMutex;
std::deque<int> sKeys;
std::atomic<bool> sWatching{false};
std::atomic<Uint64> sAskedAt{0};

bool SDLCALL Watch(void*, SDL_Event* event)
{
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat) return true;
    const int item = mscharged::platform::FriendCodeKeypadItem(event->key.scancode);
    if (item < 0 || SDL_GetTicks() - sAskedAt.load() > kAskingMs) return true;
    std::lock_guard lock(sMutex);
    if (sKeys.size() < kMaxKeys) sKeys.push_back(item);
    return true;
}
}

extern "C" int MSChargedTakeFriendCodeKey(void)
{
    if (!sWatching.exchange(true) && !SDL_AddEventWatch(Watch, nullptr))
    {
        std::fprintf(stderr, "Friend code keys unavailable: %s\n", SDL_GetError());
    }
    sAskedAt = SDL_GetTicks();
    std::lock_guard lock(sMutex);
    if (sKeys.empty()) return -1;
    const int item = sKeys.front();
    sKeys.pop_front();
    return item;
}
