#include "platform/launcher_reopen.h"
#include "platform/path.h"
#include "platform/session_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace mscharged::platform
{
namespace
{
std::filesystem::path g_program;
std::vector<std::string> g_arguments;

#ifndef _WIN32
// The game's devices, sockets and files must not stay open in the launcher:
// every descriptor above stderr closes on exec. /dev/fd lists them on Linux
// (through /proc/self/fd) and on macOS.
void CloseDescriptorsOnExec()
{
    DIR* directory = opendir("/dev/fd");
    if (!directory) return;
    const int own = dirfd(directory);
    while (const dirent* entry = readdir(directory))
    {
        char* end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || fd <= 2 || fd == own) continue;
        const int flags = fcntl(int(fd), F_GETFD);
        if (flags >= 0) fcntl(int(fd), F_SETFD, flags | FD_CLOEXEC);
    }
    closedir(directory);
}
#endif
} // namespace

void ArmLauncherReopen(const std::filesystem::path& program, int argc, char** argv)
{
    g_program = program;
    g_arguments.assign(argv, argv + argc);
}

bool ReopenLauncher(const char* reason)
{
    if (g_program.empty()) return false;
    std::error_code error;
    if (!std::filesystem::is_regular_file(g_program, error))
    {
        SessionLogLine("Cannot reopen the launcher: " + PathUtf8(g_program) + " is missing");
        return false;
    }
    EndSessionLog(reason);
#ifdef _WIN32
    std::wstring command = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(g_program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process))
    {
        std::fprintf(stderr, "Cannot reopen the launcher: %s\n",
                     std::system_category().message(int(GetLastError())).c_str());
        return true;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    std::vector<char*> arguments;
    for (auto& argument : g_arguments) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    std::fflush(nullptr);
    CloseDescriptorsOnExec();
    // The game's threads may block signals; the launcher must still get Steam's SIGTERM.
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, nullptr);
    execv(g_program.c_str(), arguments.data());
    std::fprintf(stderr, "Cannot reopen the launcher: %s\n", std::strerror(errno));
#endif
    return true;
}
} // namespace mscharged::platform
