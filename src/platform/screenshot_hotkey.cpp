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
void (*sToggleLayers)() = nullptr;
void (*sCycleLayer)() = nullptr;

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

// F8: screenshot plus a text dump of every draw of the same frame (diagnostic).
void RequestDrawDump()
{
    try
    {
        const auto directory = std::filesystem::current_path() / "screenshots";
        std::filesystem::create_directories(directory);
        auto path = NextScreenshotPath(directory, std::chrono::system_clock::now());
        RequestScreenshot();
        path.replace_extension(".draws.txt");
        const auto utf8 = path.u8string();
        const std::string text(utf8.begin(), utf8.end());
        if (aurora_debug_request_draw_dump(text.c_str()))
            std::fprintf(stderr, "Draw dump requested: %s\n", text.c_str());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Draw dump failed: %s\n", error.what());
    }
}

bool SDLCALL Watch(void*, SDL_Event* event)
{
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat && event->key.scancode == SDL_SCANCODE_P
        && event->key.windowID != 0 && event->key.windowID == sWindow.load())
        RequestScreenshot();
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat && event->key.windowID != 0
        && event->key.windowID == sWindow.load())
    {
        if (event->key.scancode == SDL_SCANCODE_F8) RequestDrawDump();
        if (event->key.scancode == SDL_SCANCODE_F9 && sToggleLayers) sToggleLayers();
        if (event->key.scancode == SDL_SCANCODE_F10 && sCycleLayer) sCycleLayer();
    }
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

void SetDebugLayerKeys(void (*toggle)(), void (*cycle)())
{
    sToggleLayers = toggle;
    sCycleLayer = cycle;
}

void ShutdownScreenshotHotkey()
{
    SetDebugLayerKeys(nullptr, nullptr);
    if (sWatching.exchange(false)) SDL_RemoveEventWatch(Watch, nullptr);
    sWindow = 0;
}
}
