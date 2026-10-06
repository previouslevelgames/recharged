#include <SDL3/SDL.h>
#include <aurora/aurora.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

// Read-only inspection of the actual SDK aliases. This fixture supplies no
// configuration provider or replacement initialization/shutdown implementation.
namespace aurora { extern AuroraConfig g_config; }

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Session
{
    bool live = false;
    ~Session() { if (live) aurora_shutdown(); }
};
}

int main(int argc, char** argv)
{
    try
    {
        const char* borrowed_base = SDL_GetBasePath();
        Require(borrowed_base != nullptr, "Missing executable directory");
        const std::string base(borrowed_base);
        const auto data = std::filesystem::path(base) / "aurora-configuration-data";
        std::filesystem::create_directories(data);
        const std::string preferences = (data / "preferences").string();

        // Explicit strings, entirely default strings, then both mixed cases.
        // Every iteration uses a real SDK session and its normal SDL shutdown.
        for (unsigned mode : {0u, 1u, 2u, 3u})
        {
            // SDL_Quit destroys its environment snapshot as well as base-path
            // storage. Select disposable Linux paths for each fresh session.
            Require(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "XDG_DATA_HOME", preferences.c_str(), true),
                    "Cannot select disposable preference directory");
            Session session;
            AuroraInfo info{};
            std::array<std::string, 4> expected;
            {
                std::array<std::string, 4> arguments{
                    "Charged native configuration ownership check",
                    (data / "user").string(), (data / "cache").string(), base + "resources"};
                AuroraConfig config{};
                const bool default_app = mode == 1 || mode == 3;
                const bool default_paths = mode == 1 || mode == 2;
                config.appName = default_app ? nullptr : arguments[0].c_str();
                config.userPath = default_paths ? nullptr : arguments[1].c_str();
                config.cachePath = default_paths ? nullptr : arguments[2].c_str();
                config.resourcesPath = default_paths ? nullptr : arguments[3].c_str();
                config.desiredBackend = BACKEND_NULL;
                config.windowWidth = 320;
                config.windowHeight = 240;
                config.windowPosX = config.windowPosY = -1;
                config.logLevel = LOG_WARNING;
                info = aurora_initialize(argc, argv, &config);
                session.live = true;
                Require(info.window != nullptr && info.backend == BACKEND_NULL, "SDK session did not initialize");
                expected[0] = default_app ? "Aurora" : arguments[0];
                char* pref = default_paths ? SDL_GetPrefPath(nullptr, expected[0].c_str()) : nullptr;
                Require(!default_paths || pref != nullptr, "Default preference path is unavailable");
#if defined(__linux__)
                Require(!default_paths || std::string(pref).starts_with(preferences + "/"),
                        "Linux preference path escaped disposable test data");
#endif
                expected[1] = default_paths ? pref : arguments[1];
                expected[2] = default_paths ? pref : arguments[2];
                expected[3] = default_paths ? base : arguments[3];
                SDL_free(pref);
                Require(aurora::g_config.appName != arguments[0].c_str(), "SDK borrowed the caller's app name");
                Require(info.userPath != arguments[1].c_str() && info.cachePath != arguments[2].c_str(),
                        "SDK borrowed the caller's paths");
                for (auto& argument : arguments) argument.assign(4096, 'x');
            } // Input buffers and the original configuration are gone.

            Require(aurora::g_config.appName && expected[0] == aurora::g_config.appName,
                    "App name did not survive caller storage destruction");
            Require(info.userPath && expected[1] == info.userPath, "Returned user path has wrong lifetime/value");
            Require(info.cachePath && expected[2] == info.cachePath, "Returned cache path has wrong lifetime/value");
            Require(aurora::g_config.resourcesPath && expected[3] == aurora::g_config.resourcesPath,
                    "Resource path has wrong lifetime/value");
            Require(expected[0] == SDL_GetWindowTitle(info.window), "SDL window did not use the requested title");
            Require(info.userPath == aurora::g_config.userPath && info.cachePath == aurora::g_config.cachePath,
                    "Returned paths do not identify the active SDK configuration");
            aurora_shutdown();
            session.live = false;
            Require(!aurora::g_config.appName && !aurora::g_config.userPath && !aurora::g_config.cachePath
                        && !aurora::g_config.resourcesPath,
                    "Shutdown retained invalid configuration aliases");
            Require(SDL_WasInit(0) == 0, "Real SDL shutdown did not run");
        }
        std::cout << "Native SDK configuration ownership passed across four explicit/default restart sessions.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
