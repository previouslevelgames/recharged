// Process setup and teardown on Horizon, in libnx's hooks around main(),
// adapted from new-coke's Strikers port (CC0).
#include "platform/switch/nx_init.h"
#include "platform/switch/sqlite_vfs.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

namespace
{
int g_nxlink = -1;
}

extern "C" void userAppInit(void)
{
    // Sockets for LAN play, and the console output under `nxlink -s`.
    if (R_SUCCEEDED(socketInitializeDefault())) g_nxlink = nxlinkStdio();

    // Started from the Album, homebrew gets a few hundred MiB: less than the
    // game and its graphics need.
    const AppletType type = appletGetAppletType();
    if (type != AppletType_Application && type != AppletType_SystemApplication)
    {
        ErrorApplicationConfig error;
        if (R_SUCCEEDED(errorApplicationCreate(&error, "Recharged needs the full memory of title mode.",
                "Recharged cannot run from the Album, which limits homebrew to a few hundred MiB.\n\n"
                "Start it from the HOME Menu instead: hold R while starting any game, then pick "
                "Recharged in the Homebrew Menu.")))
            errorApplicationShow(&error);
        std::exit(1);
    }

    // HOME > Close asks the game to close, as closing its window does on PC.
    appletLockExit();
    // Shader compilation is CPU work: run the CPU at its load clock until the
    // graphics are prepared (the GPU idles meanwhile).
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    // NVK lists only the GPUs it calls conformant without this; the Tegra X1
    // is not one of them.
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
    if (charged_switch_sqlite_vfs_register() != 0)
        std::fprintf(stderr, "SQLite file layer: registration failed; the shader cache will not be kept\n");
}

extern "C" void charged_switch_end_startup_boost(void)
{
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}

extern "C" void userAppExit(void)
{
    charged_switch_end_startup_boost();
    appletUnlockExit();
    if (g_nxlink >= 0) close(g_nxlink);
    socketExit();
}

// Threads that ask for no stack size get 8 MiB: Tint's resolver recurses
// deeply, and libnx's default is far smaller (linked with --wrap=pthread_create).
extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
    void* arg);

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
    void* arg)
{
    pthread_attr_t sized;
    std::size_t size = 0;
    if (attr != nullptr)
    {
        sized = *attr;
        pthread_attr_getstacksize(&sized, &size);
    }
    else
        pthread_attr_init(&sized);
    if (size == 0) pthread_attr_setstacksize(&sized, 8u << 20);
    return __real_pthread_create(thread, &sized, start, arg);
}

// Horizon renames only onto a free name; POSIX replaces an existing file. The
// NAND backing, settings and logs write a new file and rename it over the old
// one, as the game's saves do (linked with --wrap=rename). Not atomic here: the
// old file is removed first.
extern "C" int __real_rename(const char* old_path, const char* new_path);

extern "C" int __wrap_rename(const char* old_path, const char* new_path)
{
    if (__real_rename(old_path, new_path) == 0) return 0;
    const int error = errno;
    struct stat source{}, target{};
    if (stat(old_path, &source) != 0 || !S_ISREG(source.st_mode) || stat(new_path, &target) != 0 ||
        !S_ISREG(target.st_mode))
    {
        errno = error;
        return -1;
    }
    if (unlink(new_path) != 0) return -1;
    return __real_rename(old_path, new_path);
}

// libnx's abort exits without a report; a fault makes Atmosphère write a
// crash report with the place it happened.
extern "C" [[noreturn]] void abort(void)
{
    std::fflush(nullptr);
    *static_cast<volatile int*>(nullptr) = 0;
    for (;;) {}
}
