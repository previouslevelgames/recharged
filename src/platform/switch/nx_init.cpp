// Process setup and teardown on Horizon, in libnx's hooks around main(),
// adapted from new-coke's Strikers port (CC0).
#include "platform/switch/nx_init.h"
#include "Game/Task/GameTaskState.h"
#include "platform/switch/sqlite_vfs.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <switch.h>

namespace
{
int g_nxlink = -1;
bool g_boosted = false;

// Boost mode runs the CPU at 1785 MHz instead of 1020 and slows the GPU.
void SetCpuBoost(bool on)
{
    if (on == g_boosted) return;
    if (R_FAILED(appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal))) return;
    g_boosted = on;
    std::fprintf(stderr, "CPU boost: %s\n", on ? "on" : "off");
}
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
    SetCpuBoost(true);
    // NVK lists only the GPUs it calls conformant without this; the Tegra X1
    // is not one of them.
    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
    if (charged_switch_sqlite_vfs_register() != 0)
        std::fprintf(stderr, "SQLite file layer: registration failed; the shader cache will not be kept\n");
}

extern "C" void charged_switch_end_startup_boost(void)
{
    SetCpuBoost(false);
}

// The game's loading states show only a loading screen, which hardly uses the
// GPU, while the CPU reads and sets up the next scene.
extern "C" void charged_switch_follow_loading(unsigned task_state)
{
    constexpr unsigned kLoading =
        TASK_BOOT | TASK_CLEAN_BOOT | TASK_BOOT_TO_FE | TASK_FE_TO_GAME | TASK_GAME_TO_FE | TASK_BOOT_TO_GAME;
    SetCpuBoost((task_state & kLoading) != 0);
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

// libnx has no thread detach: newlib's pthread_detach fails (ENOSYS), so
// std::thread::detach() throws and the thread object, still joinable, ends the
// program (the freeze watch's did at the game's first frame). A detached
// thread is closed as a join closes it, by the first detach after it ended.
namespace
{
Mutex g_detached_lock;
std::vector<__pthread_t*>* g_detached = nullptr; // never freed: threads may detach while the program exits
}

extern "C" int __syscall_thread_detach(__pthread_t* thread)
{
    // The main thread has no pthread (its pthread_self() is -1).
    if (thread == nullptr || thread == reinterpret_cast<__pthread_t*>(std::intptr_t(-1))) return EINVAL;
    mutexLock(&g_detached_lock);
    int result = 0;
    try
    {
        if (g_detached == nullptr) g_detached = new std::vector<__pthread_t*>;
        auto& threads = *g_detached;
        // libnx's pthread begins with its Thread, whose handle is signalled once it has ended.
        std::size_t kept = 0;
        for (__pthread_t* detached : threads)
        {
            if (R_SUCCEEDED(svcWaitSynchronizationSingle(reinterpret_cast<Thread*>(detached)->handle, 0)))
                __syscall_thread_join(detached);
            else
                threads[kept++] = detached;
        }
        threads.resize(kept);
        threads.push_back(thread);
    }
    catch (const std::bad_alloc&)
    {
        result = ENOMEM;
    }
    mutexUnlock(&g_detached_lock);
    return result;
}

// SDL2's Switch port gives every priority but High (0x2B) Horizon's default
// 0x3B, its audio thread's time-critical one too. That thread mixes little but
// must not wait behind the game's pipeline threads (0x2C, patches/aurora-switch),
// so time-critical becomes High (linked with --wrap=SDL_SYS_SetThreadPriority).
extern "C" int __real_SDL_SYS_SetThreadPriority(int priority);

extern "C" int __wrap_SDL_SYS_SetThreadPriority(int priority)
{
    constexpr int kHigh = 2, kTimeCritical = 3; // SDL2's SDL_ThreadPriority
    return __real_SDL_SYS_SetThreadPriority(priority == kTimeCritical ? kHigh : priority);
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

// The working folder is the .nro's folder on the SD card, which libnx names
// sdmc:/switch/recharged: std::filesystem takes that for a relative path and
// absolute() puts it in front of itself. Without the device name it is the
// same folder on the card, the default device (linked with --wrap=getcwd).
extern "C" char* __real_getcwd(char* buffer, std::size_t size);

extern "C" char* __wrap_getcwd(char* buffer, std::size_t size)
{
    char* folder = __real_getcwd(buffer, size);
    if (folder != nullptr && std::strncmp(folder, "sdmc:/", 6) == 0)
        std::memmove(folder, folder + 5, std::strlen(folder + 5) + 1);
    return folder;
}

// libnx's abort exits without a report; a fault makes Atmosphère write a
// crash report with the place it happened.
extern "C" [[noreturn]] void abort(void)
{
    std::fflush(nullptr);
    *static_cast<volatile int*>(nullptr) = 0;
    for (;;) {}
}
