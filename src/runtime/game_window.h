#pragma once

#include <chrono>
#include <filesystem>
#include <string>

struct SDL_Window;

namespace mscharged::runtime {

// The game window's host behaviour around the picture (issue #91).

// While the picture keeps its 16:9 or 4:3 shape in a window, resizing the
// window keeps that shape too, so the picture fills it without bars. Off in
// fullscreen and while the picture is stretched (lock false). On macOS a
// fullscreen window at a chosen resolution is left as it is (issue #99).
void LockWindowShape(SDL_Window* window, bool lock, unsigned aspect_width, unsigned aspect_height);

// display.fullscreen_resolution on macOS: SDL places a fullscreen window at a
// chosen resolution with the screen's size from before the switch, and gives
// it its windowed size again with each size limit set later. Once macOS has
// reported the new mode, asking again places it over the whole screen (issue
// #99): call it after the window's size limits. Does nothing elsewhere, in a
// window and at the desktop's mode.
void PlaceFullscreenWindow(SDL_Window* window);

// display.position "X,Y": moves the window there when its centre then lies on
// a connected display, and on the chosen one (display.monitor, 1-based; 0 =
// any). Returns whether it moved. Never on Wayland, which places windows
// itself.
bool RestoreWindowPosition(SDL_Window* window, const std::string& position, int monitor);
// Whether this video driver lets the game read and set window positions.
bool WindowPositionsAvailable();

// display.remember_window: the window reopens where the player left it, as
// in Dolphin. The window's size and position while it is a normal window (not
// fullscreen, maximized or minimized) are saved to the settings file about a
// second after they last changed, and by Flush (when the window closes): the
// file is read again and only display.width, display.height and
// display.position change.
class WindowMemory {
public:
    using Clock = std::chrono::steady_clock;

    WindowMemory(SDL_Window* window, std::filesystem::path config);
    // Looks at the window a few times a second; saves a settled change.
    void Service(Clock::time_point now);
    // Saves a change that has not been saved yet.
    void Flush();

private:
    void Save();

    SDL_Window* window_;
    std::filesystem::path config_;
    bool positions_;
    bool known_ = false, pending_ = false, failed_ = false;
    int width_ = 0, height_ = 0, x_ = 0, y_ = 0;
    Clock::time_point changed_{}, polled_{};
};

} // namespace mscharged::runtime
