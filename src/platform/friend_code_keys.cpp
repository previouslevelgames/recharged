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

FriendCodeEdit FriendCodeEditFor(SDL_Scancode key, SDL_Keymod mods)
{
    if (key == SDL_SCANCODE_BACKSPACE || key == SDL_SCANCODE_KP_BACKSPACE) return FriendCodeEdit::Delete;
    if (key == SDL_SCANCODE_DELETE) return FriendCodeEdit::Clear;
    const bool command = (mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && !(mods & SDL_KMOD_ALT);
    if (key == SDL_SCANCODE_V && command) return FriendCodeEdit::Paste;
    if (key == SDL_SCANCODE_INSERT && (mods & SDL_KMOD_SHIFT) && !(mods & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
        return FriendCodeEdit::Paste;
    return FriendCodeEdit::None;
}

std::optional<std::array<int, 12>> ParseFriendCode(std::string_view text)
{
    std::array<int, 12> digits{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();)
    {
        const char c = text[i];
        if (c >= '0' && c <= '9')
        {
            if (count == digits.size()) return std::nullopt;
            digits[count++] = c - '0';
            ++i;
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-' || c == '.')
            ++i;
        // As text copied from a web page or a chat has them: a no-break
        // space, a narrow one, an en or em dash.
        else if (text.compare(i, 2, "\xC2\xA0") == 0)
            i += 2;
        else if (text.compare(i, 3, "\xE2\x80\xAF") == 0 || text.compare(i, 3, "\xE2\x80\x93") == 0
                 || text.compare(i, 3, "\xE2\x80\x94") == 0)
            i += 3;
        else
            return std::nullopt;
    }
    if (count != digits.size()) return std::nullopt;
    return digits;
}
}

namespace
{
using mscharged::platform::FriendCodeEdit;

// A paste and some typing; the screen asks about every 16 ms.
constexpr std::size_t kMaxKeys = 32;
constexpr Uint64 kAskingMs = 250;
// Queued by the filter, read on the game's thread: SDL calls the filter
// under its event lock, where reading the clipboard is not safe.
constexpr int kPasteRequest = 100;

std::mutex sMutex;
std::deque<int> sKeys;
// Keys the entry screen took: their repeats and release stay out of the game too.
bool sTaken[SDL_SCANCODE_COUNT]{};
std::atomic<bool> sWatching{false};
// When the screen last asked, as SDL's ticks + 1: 0 means never.
std::atomic<Uint64> sAskedAt{0};
SDL_EventFilter sPreviousFilter = nullptr;
void* sPreviousUserdata = nullptr;

bool Asking()
{
    const Uint64 asked = sAskedAt.load();
    return asked && SDL_GetTicks() + 1 - asked <= kAskingMs;
}

void Queue(int item)
{
    if (sKeys.size() < kMaxKeys) sKeys.push_back(item);
}

// The number keys reach the game as before; the screen only notes them.
bool SDLCALL Watch(void*, SDL_Event* event)
{
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat) return true;
    const int item = mscharged::platform::FriendCodeKeypadItem(event->key.scancode);
    if (item < 0 || !Asking()) return true;
    std::lock_guard lock(sMutex);
    Queue(item);
    return true;
}

// Backspace, Delete and a paste: taken from the game while the screen asks.
bool Pass(const SDL_KeyboardEvent& key)
{
    if (key.scancode <= SDL_SCANCODE_UNKNOWN || key.scancode >= SDL_SCANCODE_COUNT) return true;
    const FriendCodeEdit edit = mscharged::platform::FriendCodeEditFor(key.scancode, key.mod);
    std::lock_guard lock(sMutex);
    bool& taken = sTaken[key.scancode];
    if (!key.down)
    {
        const bool was = taken;
        taken = false;
        return !was;
    }
    const bool asking = Asking();
    if (key.repeat)
    {
        if (!taken) return true;
        // Held Backspace deletes on, as in a text field.
        if (edit == FriendCodeEdit::Delete && asking) Queue(mscharged::platform::kFriendCodeDeleteItem);
        return false;
    }
    // A new press starts over (a release lost to a focus change leaves a key marked).
    taken = asking && edit != FriendCodeEdit::None;
    if (!taken) return true;
    switch (edit)
    {
    case FriendCodeEdit::Delete: Queue(mscharged::platform::kFriendCodeDeleteItem); break;
    case FriendCodeEdit::Clear: Queue(mscharged::platform::kFriendCodeClearItem); break;
    case FriendCodeEdit::Paste: Queue(kPasteRequest); break;
    case FriendCodeEdit::None: break;
    }
    return false;
}

bool SDLCALL Filter(void*, SDL_Event* event)
{
    if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) && !Pass(event->key)) return false;
    return !sPreviousFilter || sPreviousFilter(sPreviousUserdata, event);
}

// The clipboard's code, as the clear item and its digits' keypad buttons, in
// place of the request; the refused item when it holds no code.
void ReadPaste()
{
    std::optional<std::array<int, 12>> code;
    if (char* text = SDL_GetClipboardText())
    {
        code = mscharged::platform::ParseFriendCode(text);
        SDL_free(text);
    }
    std::lock_guard lock(sMutex);
    if (sKeys.empty() || sKeys.front() != kPasteRequest) return;
    sKeys.pop_front();
    if (!code)
    {
        sKeys.push_front(mscharged::platform::kFriendCodeRefusedItem);
        return;
    }
    for (auto digit = code->rbegin(); digit != code->rend(); ++digit) sKeys.push_front(*digit == 0 ? 9 : *digit - 1);
    sKeys.push_front(mscharged::platform::kFriendCodeClearItem);
}
}

extern "C" int MSChargedTakeFriendCodeKey(void)
{
    if (!sWatching.exchange(true))
    {
        if (!SDL_AddEventWatch(Watch, nullptr)) std::fprintf(stderr, "Friend code keys unavailable: %s\n", SDL_GetError());
        // Before the first ask, so the filter passes the queued events on.
        SDL_GetEventFilter(&sPreviousFilter, &sPreviousUserdata);
        SDL_SetEventFilter(Filter, nullptr);
    }
    const Uint64 now = SDL_GetTicks() + 1;
    const Uint64 last = sAskedAt.exchange(now);
    bool paste = false;
    {
        std::lock_guard lock(sMutex);
        // Keys from before the screen asked again belong to an earlier visit.
        if (!last || now - last > kAskingMs) sKeys.clear();
        if (sKeys.empty()) return -1;
        paste = sKeys.front() == kPasteRequest;
    }
    if (paste) ReadPaste();
    std::lock_guard lock(sMutex);
    if (sKeys.empty()) return -1;
    const int item = sKeys.front();
    sKeys.pop_front();
    return item;
}
