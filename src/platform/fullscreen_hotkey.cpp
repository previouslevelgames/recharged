#include "platform/fullscreen_hotkey.h"

#include <SDL3/SDL.h>

#include <atomic>

namespace mscharged::platform
{
namespace
{
std::atomic<SDL_WindowID> sWindow{0};
// A press waiting for ServiceFullscreenHotkey.
std::atomic<bool> sPressed{false};
// The Enter keys (bit 0 Return, bit 1 keypad Enter) still held after a
// shortcut press: their auto-repeat and release stay out of the game too.
std::atomic<unsigned> sHeld{0};
// The filter set before ours, which still sees every event ours passes.
// Written only while ours is not set: SDL calls filters under its own lock.
SDL_EventFilter sPreviousFilter = nullptr;
void* sPreviousUserdata = nullptr;
bool sFiltering = false;

unsigned EnterBit(SDL_Scancode key)
{
    return key == SDL_SCANCODE_RETURN ? 1u : key == SDL_SCANCODE_KP_ENTER ? 2u : 0u;
}

// Whether the event watches and the queue may see this key event.
bool Pass(const SDL_KeyboardEvent& key)
{
    const unsigned bit = EnterBit(key.scancode);
    if (!bit || key.windowID == 0 || key.windowID != sWindow.load()) return true;
    if (!key.down) return !(sHeld.fetch_and(~bit) & bit);
    if (key.repeat) return !(sHeld.load() & bit);
    if (IsFullscreenShortcut(key.scancode, key.mod))
    {
        sHeld.fetch_or(bit);
        sPressed = true;
        return false;
    }
    // A new press: the shortcut's release was never seen (focus moved away).
    sHeld.fetch_and(~bit);
    return true;
}

bool SDLCALL Filter(void*, SDL_Event* event)
{
    if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) && !Pass(event->key)) return false;
    return !sPreviousFilter || sPreviousFilter(sPreviousUserdata, event);
}
}

bool IsFullscreenShortcut(SDL_Scancode key, SDL_Keymod modifiers)
{
    return (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) && (modifiers & SDL_KMOD_ALT)
           && !(modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI));
}

void InitializeFullscreenHotkey(SDL_Window* window)
{
    ShutdownFullscreenHotkey();
    sWindow = window ? SDL_GetWindowID(window) : 0;
    if (!sWindow.load()) return;
    SDL_GetEventFilter(&sPreviousFilter, &sPreviousUserdata);
    SDL_SetEventFilter(Filter, nullptr);
    sFiltering = true;
}

void ShutdownFullscreenHotkey()
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
    sPressed = false;
    sHeld = 0;
}

FullscreenSwitch ServiceFullscreenHotkey()
{
    if (!sPressed.exchange(false)) return FullscreenSwitch::None;
    SDL_Window* window = SDL_GetWindowFromID(sWindow.load());
    if (!window) return FullscreenSwitch::None;
    // Fullscreen covers the display at its own resolution (no mode change),
    // as at start; leaving it restores the window's size and place.
    const bool fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    if (!SDL_SetWindowFullscreen(window, fullscreen)) return FullscreenSwitch::Failed;
    return fullscreen ? FullscreenSwitch::Fullscreen : FullscreenSwitch::Windowed;
}
}
