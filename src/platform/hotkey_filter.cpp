#include "platform/hotkey_filter.h"

#include "platform/key_bindings.h"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
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

#if defined(MSCHARGED_AUTOPILOT)
// The autopilot's chord (test builds): C, P and U held together. Until a
// second of its keys comes down, a key keeps its own job, a little later: a
// hotkey on it acts on the key's release instead of its press, and a key the
// game plays with reaches the game kChordWaitMs late, so the chord's first key
// never presses a button. From the second key on, the keys of the try stay out
// of the game and their hotkeys; the chord acts once all three are down.
constexpr std::array<SDL_Scancode, 3> kChordKeys{SDL_SCANCODE_C, SDL_SCANCODE_P, SDL_SCANCODE_U};
constexpr Uint32 kChordWaitMs = 60;
// A tap released while held back still lasts this long in the game.
constexpr Uint32 kHandedBackTapMs = 30;
std::atomic<void (*)()> sChordAction{nullptr};
struct ChordKey
{
    bool down = false;
    // Its events stay out of the game until its release.
    bool taken = false;
    // Its hotkey, done at its release.
    HotkeyIndex deferred = HotkeyCount;
    // A press held back for kChordWaitMs, whether its release came meanwhile,
    // and whether the press is back in the game, its release to follow.
    SDL_TimerID wait = 0;
    SDL_Event press{};
    bool released = false;
    bool pressBack = false;
};
std::array<ChordKey, 3> sChord;
// The chord acted; it acts again once all three keys were up.
bool sChordDone = false;
// Presses and releases the chord hands back to the game, per key.
std::array<std::uint8_t, SDL_SCANCODE_COUNT> sReplays{};

int ChordIndex(SDL_Scancode key)
{
    for (int index = 0; index < 3; ++index)
        if (kChordKeys[index] == key) return index;
    return -1;
}

// A held-back press of a chord key reaches the game; when its release came
// meanwhile, the release follows kHandedBackTapMs later (the timer runs again).
Uint32 SDLCALL HandBackPress(void*, SDL_TimerID wait, Uint32)
{
    SDL_Event event{};
    Uint32 again = 0;
    {
        std::lock_guard lock(sLock);
        ChordKey* key = nullptr;
        for (ChordKey& candidate : sChord)
            if (candidate.wait == wait) key = &candidate;
        if (!key) return 0;
        event = key->press;
        if (key->pressBack)
        {
            event.key.type = SDL_EVENT_KEY_UP;
            event.key.down = false;
            *key = ChordKey{key->down};
        }
        else if (key->released)
        {
            key->pressBack = true;
            again = kHandedBackTapMs;
        }
        else
        {
            key->wait = 0;
            key->taken = false;
        }
        ++sReplays[event.key.scancode];
    }
    // Outside the lock: SDL runs our filter on the pushing thread.
    SDL_PushEvent(&event);
    return again;
}

enum class ChordPass
{
    Usual, // the usual hotkey handling decides
    Pass,
    Take,
};

ChordPass PassChordKey(const SDL_KeyboardEvent& key, int index)
{
    void (*chord)() = nullptr;
    HotkeyIndex hotkey = HotkeyCount;
    SDL_TimerID cancelled[3]{};
    ChordPass result = ChordPass::Take;
    {
        std::lock_guard lock(sLock);
        if (sReplays[key.scancode] != 0)
        {
            --sReplays[key.scancode];
            return ChordPass::Usual;
        }
        ChordKey& mine = sChord[index];
        if (key.down && key.repeat) return mine.taken ? ChordPass::Take : ChordPass::Usual;
        if (key.down)
        {
            mine.down = true;
            int down = 0;
            for (const ChordKey& other : sChord) down += other.down;
            if (down == 1)
            {
                if ((mine.deferred = sHotkeys.Match(key.scancode, key.mod)) == HotkeyCount)
                {
                    mine.press.key = key;
                    mine.released = false;
                    mine.wait = SDL_AddTimer(kChordWaitMs, HandBackPress, nullptr);
                    if (!mine.wait) return ChordPass::Usual;
                }
                mine.taken = true;
                return ChordPass::Take;
            }
            // The keys of the try stay out; one already in the game keeps its
            // release, or the game would hold it down.
            mine.taken = true;
            for (int other = 0; other < 3; ++other)
            {
                ChordKey& tried = sChord[other];
                if (!tried.down) continue;
                if (tried.wait)
                {
                    cancelled[other] = tried.wait;
                    tried.wait = 0;
                    tried.taken = true;
                }
                tried.deferred = HotkeyCount;
            }
            if (down == 3 && !sChordDone)
            {
                sChordDone = true;
                chord = sChordAction.load();
            }
        }
        else
        {
            mine.down = false;
            if (mine.wait)
            {
                // Released while held back: the press reaches the game at the end of
                // the wait, the release kHandedBackTapMs after it.
                mine.released = true;
                return ChordPass::Take;
            }
            hotkey = mine.deferred;
            mine.deferred = HotkeyCount;
            result = mine.taken ? ChordPass::Take : ChordPass::Usual;
            mine.taken = false;
            if (!sChord[0].down && !sChord[1].down && !sChord[2].down) sChordDone = false;
        }
    }
    for (const SDL_TimerID wait : cancelled)
        if (wait) SDL_RemoveTimer(wait);
    if (chord) chord();
    if (hotkey == HotkeyFullscreen)
        sFullscreenPressed = true;
    else if (hotkey != HotkeyCount)
        if (const auto action = sActions[hotkey].load()) action();
    return result;
}
#endif

// Whether the event watches and the queue may see this key event.
bool Pass(const SDL_KeyboardEvent& key)
{
    if (key.windowID == 0 || key.windowID != sWindow.load() || key.scancode <= SDL_SCANCODE_UNKNOWN
        || key.scancode >= SDL_SCANCODE_COUNT)
        return true;
#if defined(MSCHARGED_AUTOPILOT)
    if (const int chord = ChordIndex(key.scancode); chord >= 0 && sChordAction.load())
        if (const ChordPass pass = PassChordKey(key, chord); pass != ChordPass::Usual) return pass == ChordPass::Pass;
#endif
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
#if defined(MSCHARGED_AUTOPILOT)
    SDL_TimerID waits[3]{};
#endif
    {
        std::lock_guard lock(sLock);
        sHeld.reset();
#if defined(MSCHARGED_AUTOPILOT)
        // A held-back press is forgotten with the window.
        for (int index = 0; index < 3; ++index)
        {
            waits[index] = sChord[index].wait;
            sChord[index] = ChordKey{};
        }
        sChordDone = false;
        sReplays.fill(0);
#endif
    }
#if defined(MSCHARGED_AUTOPILOT)
    for (const SDL_TimerID wait : waits)
        if (wait) SDL_RemoveTimer(wait);
#endif
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

#if defined(MSCHARGED_AUTOPILOT)
void SetAutopilotChord(void (*action)())
{
    sChordAction = action;
}
#endif

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
