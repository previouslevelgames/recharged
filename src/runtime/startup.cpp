#include "runtime/startup.h"
#include "runtime/startup_animation.h"
#include "runtime/startup_files.h"
#include "runtime/events.h"
#include "runtime/tasks.h"
#include "runtime/game_config.h"
#include "runtime/tweaks.h"
#include "bootstrap/config.h"
#include "platform/disc.h"
#include "platform/path.h"
#include "mscharged/build_version.h"
#include "Game/Startup.h"
#include "Game/main.h"

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <dolphin/os.h>

#include <fstream>
#include <iostream>

namespace
{
struct Session
{
    bool initialized = false;
    bool disc = false;
    ~Session()
    {
        if (initialized) mscharged::ResetStartupFiles();
        if (disc) aurora_dvd_close();
        if (initialized) { mscharged::ResetStartupMemory(); aurora_shutdown(); }
    }
};
}

namespace mscharged
{
int RunGameStartup(int argc, char** argv, const std::filesystem::path& config_path)
{
    std::ofstream logfile;
    auto log = [&](const std::string& message) {
        std::cerr << "[startup] " << message << '\n';
        if (logfile) { logfile << "[startup] " << message << '\n'; logfile.flush(); }
    };
    std::string data_path;
    Session session;
    try
    {
        const auto file = LoadConfig(config_path);
        const auto disc_path = ResolveDiscPath(file.settings, file.path);
        const auto disc = InspectDisc(disc_path);
        if (disc.game_id != "R4QE01" || disc.revision != 1)
            throw std::runtime_error("The startup prototype currently supports R4QE01 revision 1 only.");
        if (file.settings.language != "auto" && file.settings.language != "english"
            && file.settings.language != "french" && file.settings.language != "spanish")
            throw std::runtime_error("The selected text language is not supported by this USA disc.");
        SetStartupSystemLanguage(file.settings.language == "french" ? 3 : file.settings.language == "spanish" ? 4 : 1);
        if (file.settings.language == "auto")
            log("Automatic language currently uses the USA English fallback; host-locale mapping is pending.");

        const char* base = SDL_GetBasePath();
        if (!base) throw std::runtime_error("Cannot locate the executable directory.");
        const auto data = PathFromUtf8(base) / "startup-data";
        std::filesystem::create_directories(data);
        logfile.open(data / "startup.log", std::ios::trunc);
        if (!logfile) throw std::runtime_error("Cannot create the startup diagnostic log.");
        log(std::string("mscharged ") + build::version + "; decomp " + MSCHARGED_DECOMP_REVISION);
        log(disc.game_id + " revision " + std::to_string(disc.revision) + " (" + disc.format
            + "), " + std::to_string(disc.file_count) + " files; original startup prototype, not playable.");
        data_path = PathUtf8(data);
        AuroraConfig aurora{};
        aurora.appName = "Mario Strikers Charged | Experimental startup";
        aurora.userPath = data_path.c_str();
        aurora.cachePath = data_path.c_str();
        aurora.resourcesPath = base;
        aurora.desiredBackend = BACKEND_NULL; // No GX renderer is connected in this prototype.
        aurora.windowWidth = 800;
        aurora.windowHeight = 600;
        aurora.windowPosX = aurora.windowPosY = -1;
        aurora.logLevel = LOG_WARNING;
        aurora.mem1Size = MEM1_DEFAULT_SIZE;
        aurora.mem2Size = 64 * 1024 * 1024;

        log("Initializing Aurora core (null backend; game graphics pending).");
        const auto info = aurora_initialize(argc, argv, &aurora);
        session.initialized = true;
        if (!info.window || info.backend != BACKEND_NULL)
            throw std::runtime_error("Aurora did not initialize the requested core-only configuration.");
        for (const auto* event = aurora_update(); event->type != AURORA_NONE; ++event)
            if (event->type == AURORA_EXIT) throw std::runtime_error("Startup cancelled.");
        InitializeStartupOS();
        log("Aurora MEM1 and OS clock initialized.");
        if (!aurora_dvd_open(PathUtf8(disc_path).c_str()))
            throw std::runtime_error("Aurora DVD could not open the Wii game data partition.");
        session.disc = true;
        log("Wii image mounted through Aurora DVD/nod.");
        g_Region = 0;
        log("Entering original InitializeCore() -> nlInit() -> nlInitMemory().");
        InitializeCore();
        log("Original InitializeCore() and nlInit() completed.");
        log(VerifyStartupEvents());
        log(VerifyStartupQueuedEvents());
        log(VerifyStartupTaskScheduler());
        VerifyStartupFileReads();
        log(VerifyStartupAnimationDecoders());
        log(VerifyStartupWholeFileLoads());
        log(VerifyStartupConfig());
        log(VerifyStartupTweaks());
        // Until the remaining main.cpp initialization can be linked, do not
        // manufacture a game loop if this prefix becomes fully implemented.
        MissingStartupService("Initialize (remaining stages)",
            "The rest of original game initialization and task loop are not linked yet.");
    }
    catch (const StartupStopped& error)
    {
        log(StartupMemorySummary());
        log(StartupFileSummary());
        log("Original text language ID: " + std::to_string(static_cast<int>(g_Language)));
        log(std::string("STOPPED at unimplemented service: ") + error.what());
        log("No menu or match was reached. Exit code 3 identifies this development boundary.");
        return 3;
    }
    catch (const std::exception& error)
    {
        log(std::string("FAILED: ") + error.what());
        return 1;
    }
}
}
