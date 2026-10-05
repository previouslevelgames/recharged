#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/time.hpp>
#include <dolphin/os.h>

#include "mscharged/build_version.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Session
{
    bool initialized = false;
    ~Session() { if (initialized) aurora_shutdown(); }
};

bool Update()
{
    bool exit = false;
    for (const AuroraEvent* event = aurora_update(); event->type != AURORA_NONE; ++event)
        exit |= event->type == AURORA_EXIT;
    return exit;
}
}

int main(int argc, char** argv)
{
    bool window = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--window") window = true;
        else if (argument == "--help")
        {
            std::cout << "Usage: mscharged-aurora-check [--window | --version]\n"
                         "Checks Aurora core initialization without game data or GX rendering.\n"
                         "--window keeps the host-check window open until it is closed.\n";
            return 0;
        }
        else if (argument == "--version")
        {
            std::cout << "mscharged Aurora host check " << mscharged::build::version << '\n';
            return 0;
        }
        else { std::cerr << "Unknown argument: " << argument << '\n'; return 2; }
    }

    try
    {
        const char* base = SDL_GetBasePath();
        Require(base != nullptr, "Cannot find the executable directory");
        const std::string base_bytes(base);
        const std::u8string base_utf8(base_bytes.begin(), base_bytes.end());
        const auto data = std::filesystem::path(base_utf8) / "aurora-check-data";
        std::filesystem::create_directories(data);
        const auto encoded = data.u8string();
        const std::string data_path(encoded.begin(), encoded.end());
        AuroraConfig config{};
        config.appName = "Charged - Aurora host check (GX pending)";
        config.userPath = data_path.c_str();
        config.cachePath = data_path.c_str();
        config.resourcesPath = base;
        config.desiredBackend = BACKEND_NULL; // Explicit core-only configuration, not a GPU fallback.
        config.windowWidth = 800;
        config.windowHeight = 600;
        config.windowPosX = config.windowPosY = -1;
        config.logLevel = LOG_WARNING;
        config.mem1Size = MEM1_DEFAULT_SIZE;
        config.mem2Size = 0; // Wii MEM2 integration is not established by this check.

        Session session;
        const auto info = aurora_initialize(argc, argv, &config);
        session.initialized = true;
        Require(info.window != nullptr && info.backend == BACKEND_NULL, "Unexpected Aurora host initialization result");
        Require(info.windowSize.width > 0 && info.windowSize.height > 0, "Aurora returned an empty window");
        Require(!aurora_get_last_presentation().sequence, "GX-disabled host invented a game presentation");
        Update(); // Aurora initializes controllers on its first update.
        Require((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) != 0, "Controller subsystem did not initialize");

        OSInit();
        Require(OSGetPhysicalMemSize() == MEM1_DEFAULT_SIZE, "MEM1 boot information is incorrect");
        const auto low = reinterpret_cast<std::uintptr_t>(OSGetArenaLo());
        const auto high = reinterpret_cast<std::uintptr_t>(OSGetArenaHi());
        Require(low != 0 && high > low + 256, "MEM1 arena is unavailable");
        auto* allocation = static_cast<unsigned char*>(OSAllocFromArenaLo(256, 32));
        const auto address = reinterpret_cast<std::uintptr_t>(allocation);
        Require(address >= low && address + 256 <= high && address % 32 == 0, "MEM1 arena allocation is invalid");
        std::fill_n(allocation, 256, 0x5a);
        Require(std::all_of(allocation, allocation + 256, [](unsigned char byte) { return byte == 0x5a; }),
                "MEM1 allocation is not writable");

        const auto before = OSGetTime();
        SDL_Delay(10);
        Require(OSGetTime() > before, "Aurora OS clock did not advance");
        aurora_set_timescale(0);
        const auto paused = aurora::time::game_clock::now();
        SDL_Delay(3);
        Require(aurora::time::game_clock::now() == paused, "Game clock did not pause");
        aurora_set_timescale(1);

        auto* renderer = SDL_GetRenderer(info.window);
        Require(renderer != nullptr, "Aurora did not create its core-only SDL renderer");
        for (unsigned frame = 0; window || frame < 3; ++frame)
        {
            if (Update()) break;
            Require(aurora_begin_frame(), "Aurora host frame did not begin");
            SDL_SetRenderDrawColor(renderer, 17, 21, 29, 255);
            Require(SDL_RenderClear(renderer), "Cannot clear host-check window");
            SDL_SetRenderScale(renderer, 2, 2);
            SDL_SetRenderDrawColor(renderer, 225, 235, 250, 255);
            SDL_RenderDebugText(renderer, 16, 20, "Aurora host services running");
            SDL_RenderDebugText(renderer, 16, 44, "Window / events / input initialization");
            SDL_RenderDebugText(renderer, 16, 60, "MEM1 arena / clock / pause checks passed");
            SDL_SetRenderDrawColor(renderer, 250, 186, 95, 255);
            SDL_RenderDebugText(renderer, 16, 92, "GX/Vulkan and game startup are pending.");
            SDL_SetRenderScale(renderer, 1, 1);
            aurora_end_frame();
            Require(SDL_RenderPresent(renderer), "Cannot present host-check window");
            SDL_Delay(16);
        }
        if (!window)
        {
            SDL_Event exit_event{};
            exit_event.type = SDL_EVENT_QUIT;
            Require(SDL_PushEvent(&exit_event), "Cannot inject exit event");
            Require(Update(), "Aurora did not forward the exit event");
        }
        Require(!aurora_get_last_presentation().sequence, "Core-only SDL rendering published a GX presentation");
        std::cout << "Aurora host checks passed: window, event loop, controller initialization, MEM1 and clocks.\n"
                     "GX/Vulkan rendering, Wii MEM2/input/audio and game startup remain pending.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Aurora host check failed: " << error.what() << " (SDL: " << SDL_GetError() << ")\n";
        return 1;
    }
}
