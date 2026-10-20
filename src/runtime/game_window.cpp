#include "runtime/game_window.h"

#include "bootstrap/config.h"
#include "platform/path.h"
#include "platform/session_log.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <exception>
#include <utility>

namespace mscharged::runtime {
namespace {
// Polls of the window, and how long a change must last before it is saved.
constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr auto kSettleTime = std::chrono::seconds(1);
}

void LockWindowShape(SDL_Window* window, bool lock, unsigned aspect_width, unsigned aspect_height)
{
    if (!window) return;
    const float aspect = lock && aspect_height ? float(aspect_width) / float(aspect_height) : 0.0f;
    if (!SDL_SetWindowAspectRatio(window, aspect, aspect))
        platform::SessionLogLine(std::string("Display: the window cannot keep the picture's shape: ") + SDL_GetError());
}

bool WindowPositionsAvailable()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && std::strcmp(driver, "wayland") != 0;
}

bool RestoreWindowPosition(SDL_Window* window, const std::string& position, int monitor)
{
    const auto point = ParseWindowPosition(position);
    if (!window || !point || !WindowPositionsAvailable()) return false;
    int width = 0, height = 0;
    if (!SDL_GetWindowSize(window, &width, &height)) return false;
    const SDL_Rect rect{(*point)[0], (*point)[1], width, height};
    // The display under the window's centre; SDL names the closest one even
    // for a rectangle on no display, which this checks.
    const SDL_DisplayID display = SDL_GetDisplayForRect(&rect);
    SDL_Rect bounds{}, usable{};
    if (!display || !SDL_GetDisplayBounds(display, &bounds) || !SDL_GetDisplayUsableBounds(display, &usable))
        return false;
    const SDL_Point centre{rect.x + width / 2, rect.y + height / 2};
    if (!SDL_PointInRect(&centre, &bounds) || rect.y < usable.y) return false;
    if (monitor > 0) {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        const bool chosen = displays && monitor <= count && displays[monitor - 1] == display;
        SDL_free(displays);
        if (!chosen) return false;
    }
    return SDL_SetWindowPosition(window, rect.x, rect.y);
}

WindowMemory::WindowMemory(SDL_Window* window, std::filesystem::path config)
    : window_(window), config_(std::move(config)), positions_(WindowPositionsAvailable())
{
}

void WindowMemory::Service(Clock::time_point now)
{
    if (!window_ || failed_) return;
    if (pending_ && now - changed_ >= kSettleTime) Save();
    if (now - polled_ < kPollInterval) return;
    polled_ = now;
    const auto flags = SDL_GetWindowFlags(window_);
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) return;
    int width = 0, height = 0, x = 0, y = 0;
    if (!SDL_GetWindowSize(window_, &width, &height) || width <= 0 || height <= 0) return;
    if (positions_ && !SDL_GetWindowPosition(window_, &x, &y)) return;
    if (known_ && width == width_ && height == height_ && x == x_ && y == y_) return;
    // The first look is where the window opened: nothing to save yet.
    pending_ = known_;
    known_ = true;
    width_ = width;
    height_ = height;
    x_ = x;
    y_ = y;
    changed_ = now;
}

void WindowMemory::Flush()
{
    if (pending_ && !failed_) Save();
}

void WindowMemory::Save()
{
    pending_ = false;
    try {
        auto file = LoadConfig(config_);
        auto settings = file.settings;
        settings.width = width_;
        settings.height = height_;
        if (positions_) settings.position = FormatWindowPosition(x_, y_);
        if (settings.width == file.settings.width && settings.height == file.settings.height
            && settings.position == file.settings.position)
            return;
        SaveConfig(file, settings);
        platform::SessionLogLine("Display: window remembered at " + std::to_string(width_) + "x"
            + std::to_string(height_) + (positions_ ? " (" + settings.position + ")" : std::string()));
    } catch (const std::exception& error) {
        // A settings file that cannot be read or written keeps its contents.
        failed_ = true;
        platform::SessionLogLine(std::string("Display: the window's size and position cannot be saved: ")
            + error.what());
    }
}

} // namespace mscharged::runtime
