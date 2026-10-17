#include "platform/log_window.h"

#include "platform/path.h"
#include "platform/session_log.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <climits>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace mscharged::platform
{
namespace
{
constexpr std::string_view kFlag = "--log-window";
constexpr auto kPoll = std::chrono::milliseconds(250);
// The last state SetLogWindow was asked for; only changes act.
bool g_requested = false;

#ifdef _WIN32
bool Usable(DWORD which)
{
    const HANDLE handle = GetStdHandle(which);
    return handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

// Opens `device` as `stream` on descriptor `fd`. Without a console the C
// runtime holds fds 0-2 as "no console" (-2): closing one frees its number,
// and freopen takes the lowest free one, so the streams keep 0, 1 and 2.
void Bind(FILE* stream, int fd, DWORD which, const char* device, const char* mode)
{
    _close(fd);
    if (!std::freopen(device, mode, stream)) return;
    if (_fileno(stream) != fd) _dup2(_fileno(stream), fd);
    SetStdHandle(which, HANDLE(_get_osfhandle(fd)));
}

// Closing a "no console" descriptor counts as an invalid parameter, which
// would otherwise end the program.
void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}

std::wstring Quoted(const std::wstring& path) { return L"\"" + path + L"\""; }

HANDLE g_window = nullptr; // the log window's process

void OpenWindow(const fs::path& log)
{
    std::wstring program(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, program.data(), DWORD(program.size()));
    if (!length || length >= program.size()) return;
    program.resize(length);
    std::wstring command = Quoted(program) + L" --log-window " + Quoted(log.wstring()) + L" "
        + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr,
                        &startup, &process))
    {
        SessionLogLine("Log window: cannot start it (" + std::system_category().message(int(GetLastError())) + ")");
        return;
    }
    CloseHandle(process.hThread);
    g_window = process.hProcess;
}

void CloseWindow()
{
    if (!g_window) return;
    TerminateProcess(g_window, 0);
    CloseHandle(g_window);
    g_window = nullptr;
}

// Shared with the game, which keeps writing; the next run may rename it.
class LogFile
{
public:
    explicit LogFile(const fs::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    ~LogFile()
    {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    // Appends what was written since the last call.
    void Read(std::string& text)
    {
        char buffer[65536];
        DWORD read = 0;
        while (ReadFile(handle_, buffer, sizeof buffer, &read, nullptr) && read) text.append(buffer, read);
    }

private:
    HANDLE handle_;
};

class Console
{
public:
    Console()
    {
        AllocConsole();
        SetConsoleTitleW(L"Recharged log");
        output_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    }
    // Text is UTF-8; the console takes UTF-16.
    void Write(std::string_view text)
    {
        if (output_ == INVALID_HANDLE_VALUE || text.empty()) return;
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
        std::wstring wide(std::size_t(size > 0 ? size : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
        DWORD written = 0;
        WriteConsoleW(output_, wide.data(), DWORD(wide.size()), &written, nullptr);
    }
    void WaitForKey()
    {
        const HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, 0, nullptr);
        if (input == INVALID_HANDLE_VALUE) return;
        FlushConsoleInputBuffer(input);
        INPUT_RECORD record;
        DWORD count = 0;
        while (ReadConsoleInputW(input, &record, 1, &count))
            if (count && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) break;
        CloseHandle(input);
    }

private:
    HANDLE output_ = INVALID_HANDLE_VALUE;
};

class Game
{
public:
    explicit Game(unsigned long id) : handle_(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(id))) {}
    ~Game()
    {
        if (handle_) CloseHandle(handle_);
    }
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;
    // Waits up to one poll interval.
    bool Running() { return handle_ && WaitForSingleObject(handle_, DWORD(kPoll.count())) == WAIT_TIMEOUT; }
    DWORD ExitCode()
    {
        DWORD code = 0;
        if (handle_) GetExitCodeProcess(handle_, &code);
        return code;
    }

private:
    HANDLE handle_;
};
#else
// Shared with the game, which keeps writing.
class LogFile
{
public:
    explicit LogFile(const fs::path& path) : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {}
    ~LogFile()
    {
        if (fd_ >= 0) ::close(fd_);
    }
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
    bool IsOpen() const { return fd_ >= 0; }
    // Appends what was written since the last call.
    void Read(std::string& text)
    {
        char buffer[65536];
        for (;;)
        {
            const ssize_t read = ::read(fd_, buffer, sizeof buffer);
            if (read > 0) text.append(buffer, std::size_t(read));
            else if (read < 0 && errno == EINTR) continue;
            else break;
        }
    }

private:
    int fd_;
};

class Console
{
public:
    Console() { Write("\033]0;Recharged log\007"); }
    void Write(std::string_view text)
    {
        while (!text.empty())
        {
            const ssize_t written = ::write(STDOUT_FILENO, text.data(), text.size());
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return;
            text.remove_prefix(std::size_t(written));
        }
    }
};

class Game
{
public:
    explicit Game(unsigned long id) : id_(pid_t(id)) {}
    // Waits one poll interval first.
    bool Running()
    {
        std::this_thread::sleep_for(kPoll);
        return id_ > 0 && (::kill(id_, 0) == 0 || errno == EPERM);
    }

private:
    pid_t id_;
};
#endif

#ifdef __APPLE__
std::string ShellQuoted(const fs::path& path)
{
    std::string quoted = "'";
    for (const char c : PathUtf8(path))
    {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
}

fs::path g_command; // logs/log-window.command while the window is wanted

// The log window runs in Terminal (or whichever app opens .command files):
// `open` runs a small script there that starts this program's viewer.
void OpenWindow(const fs::path& log)
{
    char buffer[PATH_MAX];
    std::uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0) return;
    std::error_code error;
    const fs::path program = fs::canonical(buffer, error);
    if (error) return;
    const fs::path command = log.parent_path() / "log-window.command";
    {
        std::ofstream file(command, std::ios::trunc);
        file << "#!/bin/sh\n# Recharged's log window (Advanced > Log window). It ends with the game.\nexec "
             << ShellQuoted(program) << " --log-window " << ShellQuoted(log) << ' ' << ::getpid() << ' '
             << ShellQuoted(command) << '\n';
        if (!file.flush())
        {
            SessionLogLine("Log window: cannot write " + PathUtf8(command));
            return;
        }
    }
    fs::permissions(command, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                    | fs::perms::others_read | fs::perms::others_exec, error);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addinherit_np(&actions, STDOUT_FILENO); // its messages reach the log
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    const std::string path = PathUtf8(command);
    char* arguments[] = {const_cast<char*>("open"), const_cast<char*>(path.c_str()), nullptr};
    pid_t opener = 0;
    const int failed = posix_spawn(&opener, "/usr/bin/open", &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (failed)
    {
        SessionLogLine("Log window: cannot run open (" + std::system_category().message(failed) + ")");
        return;
    }
    std::thread([opener] {
        int status = 0;
        waitpid(opener, &status, 0);
    }).detach();
    g_command = command;
}

// The viewer ends once its script is gone.
void CloseWindow()
{
    if (g_command.empty()) return;
    std::error_code error;
    fs::remove(g_command, error);
    g_command.clear();
}
#endif
} // namespace

void ConnectStandardStreams()
{
#ifdef _WIN32
    const bool in = Usable(STD_INPUT_HANDLE), out = Usable(STD_OUTPUT_HANDLE), err = Usable(STD_ERROR_HANDLE);
    if (out && err) return; // a console of its own, or redirected to files or pipes
    const auto previous = _set_invalid_parameter_handler(IgnoreInvalidParameter);
    const char* device = AttachConsole(ATTACH_PARENT_PROCESS) ? "CONOUT$" : "NUL";
    // In order, so each stream gets its own descriptor back.
    if (!in) Bind(stdin, 0, STD_INPUT_HANDLE, "NUL", "r");
    if (!out) Bind(stdout, 1, STD_OUTPUT_HANDLE, device, "w");
    if (!err) Bind(stderr, 2, STD_ERROR_HANDLE, device, "w");
    _set_invalid_parameter_handler(previous);
#endif
}

void SetLogWindow(bool open)
{
    if (open == g_requested) return;
    g_requested = open;
#if defined(_WIN32) || defined(__APPLE__)
    CloseWindow();
    if (!open) return;
    const fs::path log = SessionLogPath();
    if (!log.empty()) OpenWindow(log);
#endif
}

bool IsLogWindowProcess(int argc, char** argv) { return argc >= 4 && argv[1] == kFlag; }

int RunLogWindow(int argc, char** argv)
{
    const fs::path log = PathFromUtf8(argv[2]);
    const fs::path stop = argc >= 5 ? PathFromUtf8(argv[4]) : fs::path();
    unsigned long id = 0;
    try { id = std::stoul(argv[3]); }
    catch (const std::exception&) {}
    Console console;
    LogFile file(log);
    if (!file.IsOpen())
    {
        console.Write("Recharged log: cannot open " + PathUtf8(log) + "\n");
        return 1;
    }
    console.Write("Recharged log: " + PathUtf8(log) + "\nClosing this window does not stop the game.\n\n");
    Game game(id);
    std::string pending;
    bool turned_off = false;
    // Whole lines only, so a character is never split between two reads.
    const auto show = [&](bool everything) {
        file.Read(pending);
        const std::size_t newline = pending.rfind('\n');
        const std::size_t end = everything ? pending.size() : newline == std::string::npos ? 0 : newline + 1;
        if (!end) return;
        console.Write(std::string_view(pending).substr(0, end));
        pending.erase(0, end);
    };
    for (;;)
    {
        show(false);
        if (!game.Running()) break;
        std::error_code error;
        if (!stop.empty() && !fs::exists(stop, error))
        {
            turned_off = true;
            break;
        }
    }
    show(true);
    if (turned_off)
    {
        console.Write("\nThe log window was turned off in the launcher.\n");
        return 0;
    }
#ifdef _WIN32
    if (const DWORD code = game.ExitCode())
    {
        char text[160];
        std::snprintf(text, sizeof text,
                      "\nRecharged stopped with an error (exit code 0x%08lX). Press any key to close this window.\n",
                      static_cast<unsigned long>(code));
        console.Write(text);
        console.WaitForKey();
    }
#else
    console.Write("\nRecharged has closed.\n");
    std::error_code error;
    if (!stop.empty()) fs::remove(stop, error);
#endif
    return 0;
}
} // namespace mscharged::platform
