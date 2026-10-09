#include "platform/screenshot_hotkey.h"

#include "platform/screenshot_path.h"

#include <SDL3/SDL.h>
#include <aurora/video.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

namespace mscharged::platform
{
namespace
{
std::atomic<SDL_WindowID> sWindow{0};
std::atomic<bool> sWatching{false};

void RequestScreenshot()
{
    try
    {
        const auto directory = std::filesystem::current_path() / "screenshots";
        std::filesystem::create_directories(directory);
        const auto utf8 = NextScreenshotPath(directory, std::chrono::system_clock::now()).u8string();
        const std::string path(utf8.begin(), utf8.end());
        if (!aurora_request_video_screenshot(path.c_str()))
            std::fprintf(stderr, "Screenshot skipped: the previous one is still waiting for its frame.\n");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Screenshot failed: %s\n", error.what());
    }
}

bool SDLCALL Watch(void*, SDL_Event* event)
{
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat && event->key.scancode == SDL_SCANCODE_P
        && event->key.windowID != 0 && event->key.windowID == sWindow.load())
        RequestScreenshot();
    return true;
}
}

void InitializeScreenshotHotkey(SDL_Window* window)
{
    sWindow = window ? SDL_GetWindowID(window) : 0;
    if (!sWatching.exchange(true) && !SDL_AddEventWatch(Watch, nullptr))
    {
        sWatching = false;
        std::fprintf(stderr, "Screenshot key unavailable: %s\n", SDL_GetError());
    }
}

void ShutdownScreenshotHotkey()
{
    if (sWatching.exchange(false)) SDL_RemoveEventWatch(Watch, nullptr);
    sWindow = 0;
}
}
