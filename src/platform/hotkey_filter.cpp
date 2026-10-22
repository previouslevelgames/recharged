#include "platform/hotkey_filter.h"

#include "platform/key_bindings.h"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <bitset>
#include <mutex>

namespace mscharged::platform
{
namespace
{
std::atomic<SDL_WindowID> sWindow{0};
std::array<std::atomic<void (*)()>, HotkeyCount> sActions{};
// A fullscreen press waiting for ServiceFullscreenHotkey.
std::atomic<bool> sFullscreenPressed{false};
// The hotkeys, and the keys still held after a hotkey press: their repeats
// and release stay out of the game too.
std::mutex sLock;
Hotkeys sHotkeys;
std::bitset<SDL_SCANCODE_COUNT> sHeld;
// The filter set before ours, which still sees every event ours passes.
// Written only while ours is not set: SDL calls filters under its own lock.
SDL_EventFilter sPreviousFilter = nullptr;
void* sPreviousUserdata = nullptr;
bool sFiltering = false;

// Whether the event watches and the queue may see this key event.
bool Pass(const SDL_KeyboardEvent& key)
{
    if (key.windowID == 0 || key.windowID != sWindow.load() || key.scancode <= SDL_SCANCODE_UNKNOWN
        || key.scancode >= SDL_SCANCODE_COUNT)
        return true;
    HotkeyIndex hotkey = HotkeyCount;
    {
        std::lock_guard lock(sLock);
        const bool held = sHeld[key.scancode];
        if (!key.down)
        {
            sHeld[key.scancode] = false;
            return !held;
        }
        if (key.repeat) return !held;
        hotkey = sHotkeys.Match(key.scancode, key.mod);
        // Otherwise a new press: the hotkey's release was never seen (focus moved away).
        sHeld[key.scancode] = hotkey != HotkeyCount;
    }
    if (hotkey == HotkeyCount) return true;
    if (hotkey == HotkeyFullscreen) sFullscreenPressed = true;
    else if (const auto action = sActions[hotkey].load()) action();
    return false;
}

bool SDLCALL Filter(void*, SDL_Event* event)
{
    if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) && !Pass(event->key)) return false;
    return !sPreviousFilter || sPreviousFilter(sPreviousUserdata, event);
}
}

void InitializeHotkeys(SDL_Window* window)
{
    ShutdownHotkeys();
    sWindow = window ? SDL_GetWindowID(window) : 0;
    if (!sWindow.load()) return;
    SDL_GetEventFilter(&sPreviousFilter, &sPreviousUserdata);
    SDL_SetEventFilter(Filter, nullptr);
    sFiltering = true;
}

void ShutdownHotkeys()
{
    if (sFiltering)
    {
        SDL_EventFilter current = nullptr;
        void* userdata = nullptr;
        // A filter set after ours stays; it calls whatever it replaced.
        if (SDL_GetEventFilter(&current, &userdata) && current == Filter)
            SDL_SetEventFilter(sPreviousFilter, sPreviousUserdata);
        sFiltering = false;
    }
    sPreviousFilter = nullptr;
    sPreviousUserdata = nullptr;
    sWindow = 0;
    sFullscreenPressed = false;
    std::lock_guard lock(sLock);
    sHeld.reset();
}

void SetHotkeys(const Hotkeys& hotkeys)
{
    std::lock_guard lock(sLock);
    sHotkeys = hotkeys;
}

void SetHotkeyAction(HotkeyIndex hotkey, void (*action)())
{
    if (hotkey < HotkeyCount) sActions[hotkey] = action;
}

FullscreenSwitch ServiceFullscreenHotkey()
{
    if (!sFullscreenPressed.exchange(false)) return FullscreenSwitch::None;
    SDL_Window* window = SDL_GetWindowFromID(sWindow.load());
    if (!window) return FullscreenSwitch::None;
    // Fullscreen covers the display at its own resolution (no mode change),
    // as at start; leaving it restores the window's size and place.
    const bool fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    if (!SDL_SetWindowFullscreen(window, fullscreen)) return FullscreenSwitch::Failed;
    return fullscreen ? FullscreenSwitch::Fullscreen : FullscreenSwitch::Windowed;
}
}
