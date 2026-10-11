#include "platform/session_log.h"
#include "platform/path.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define MSCHARGED_HAS_BACKTRACE 1
#endif
#endif

namespace mscharged::platform
{
namespace
{
namespace fs = std::filesystem;

#ifdef _WIN32
int Write(int fd, const void* data, std::size_t size) { return _write(fd, data, unsigned(size)); }
#else
int Write(int fd, const void* data, std::size_t size) { return int(::write(fd, data, size)); }
#endif

// One copied stream: the pipe that replaced stdout or stderr, the console it
// came from and whether its reader is copying a chunk right now.
struct Stream
{
    int pipe_read = -1;
    int console = -1;
    std::atomic<bool> busy{false};
};

struct Log
{
    fs::path path, dump;
    int file = -1;
    std::mutex mutex; // whole chunks, so stdout and stderr lines do not mix
    Stream out, err;
    bool started = false;
} g_log;

void WriteLog(const char* data, std::size_t size)
{
    if (g_log.file < 0 || !size) return;
    std::lock_guard lock(g_log.mutex);
    (void)Write(g_log.file, data, size);
}

// Crash handlers cannot take the mutex (the crashing thread may hold it).
void WriteLogUnlocked(const char* data, std::size_t size)
{
    if (g_log.file >= 0) (void)Write(g_log.file, data, size);
}

void Reader(Stream* stream)
{
    char buffer[4096];
    for (;;)
    {
#ifdef _WIN32
        const int got = _read(stream->pipe_read, buffer, sizeof buffer);
#else
        const auto got = ::read(stream->pipe_read, buffer, sizeof buffer);
#endif
        if (got <= 0) return;
        stream->busy = true;
        if (stream->console >= 0) (void)Write(stream->console, buffer, std::size_t(got));
        WriteLog(buffer, std::size_t(got));
        stream->busy = false;
    }
}

std::size_t Pending(const Stream& stream)
{
    if (stream.pipe_read < 0) return 0;
#ifdef _WIN32
    DWORD available = 0;
    if (!PeekNamedPipe(HANDLE(_get_osfhandle(stream.pipe_read)), nullptr, 0, nullptr, &available, nullptr)) return 0;
    return available;
#else
    int available = 0;
    if (ioctl(stream.pipe_read, FIONREAD, &available) != 0) return 0;
    return std::size_t(available);
#endif
}

// Waits (up to a second) until the readers have copied what was written.
void Drain()
{
    for (int n = 0; n < 1000; ++n)
    {
        if (!Pending(g_log.out) && !Pending(g_log.err) && !g_log.out.busy && !g_log.err.busy) return;
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
}

// Replaces file descriptor `fd` (1 or 2) by a pipe whose reader copies to
// the original console and the log.
bool Redirect(int fd, Stream& stream)
{
    int ends[2];
#ifdef _WIN32
    if (_pipe(ends, 65536, _O_BINARY) != 0) return false;
    stream.console = _dup(fd); // -1 without a console
    if (_dup2(ends[1], fd) != 0) { _close(ends[0]); _close(ends[1]); return false; }
    _close(ends[1]);
    SetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, HANDLE(_get_osfhandle(fd)));
#else
    if (pipe(ends) != 0) return false;
    stream.console = dup(fd);
    if (dup2(ends[1], fd) < 0) { close(ends[0]); close(ends[1]); return false; }
    close(ends[1]);
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
#endif
    stream.pipe_read = ends[0];
    std::thread(Reader, &stream).detach();
    return true;
}

std::string Utf8(const fs::path& path) { return PathUtf8(path); }

std::string Line(const char* format, ...)
{
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    return buffer;
}

void Header(const char* version, int argc, char** argv)
{
    std::string text = Line("=== Mario Strikers Charged native port %s ===\n", version);
    const std::time_t now = std::time(nullptr);
    char stamp[64] = "";
    if (const std::tm* local = std::localtime(&now)) std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S %z", local);
    text += Line("Started: %s\n", stamp);
    text += "Command line:";
    for (int n = 0; n < argc; ++n) text += std::string(" ") + argv[n];
    text += "\n";
#ifdef _WIN32
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof os;
    if (const auto get = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        get(&os);
    text += Line("OS: Windows %lu.%lu build %lu\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    char cpu[256] = "";
    DWORD size = sizeof cpu;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &size) == ERROR_SUCCESS)
        text += Line("CPU: %s\n", cpu);
#else
    utsname name{};
    if (uname(&name) == 0) text += Line("OS: %s %s %s\n", name.sysname, name.release, name.machine);
#if defined(__APPLE__)
    char product[64] = "", cpu[256] = "";
    std::size_t size = sizeof product;
    if (sysctlbyname("kern.osproductversion", product, &size, nullptr, 0) == 0) text += Line("macOS: %s\n", product);
    size = sizeof cpu;
    if (sysctlbyname("machdep.cpu.brand_string", cpu, &size, nullptr, 0) == 0) text += Line("CPU: %s\n", cpu);
#else
    std::ifstream release("/etc/os-release");
    for (std::string line; std::getline(release, line);)
        if (line.rfind("PRETTY_NAME=", 0) == 0) text += "Distribution: " + line.substr(12) + "\n";
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);)
        if (line.rfind("model name", 0) == 0)
        {
            text += "CPU:" + line.substr(line.find(':') + 1) + "\n";
            break;
        }
#endif
#endif
    text += Line("Logical CPUs: %d, memory: %d MB\n", SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());
    text += "Log: " + Utf8(g_log.path) + "\n\n";
    WriteLog(text.data(), text.size());
}

// --- Crashes -------------------------------------------------------------

std::atomic<bool> g_crashing{false};

void CrashWrite(const char* text)
{
    const std::size_t size = std::strlen(text);
    WriteLogUnlocked(text, size);
    if (g_log.err.console >= 0) (void)Write(g_log.err.console, text, size);
}

#ifdef _WIN32
// "module.dll+0x1234" for a code address.
void Location(DWORD64 address, char* out, std::size_t size)
{
    HMODULE module = nullptr;
    wchar_t wide[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module))
        GetModuleFileNameW(module, wide, MAX_PATH);
    const wchar_t* base = wcsrchr(wide, L'\\');
    char name[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : wide, -1, name, sizeof name, nullptr, nullptr);
    std::snprintf(out, size, "%s+0x%llx", name,
                  (unsigned long long)(address - reinterpret_cast<DWORD64>(module)));
}

void WriteStack(CONTEXT context)
{
    CrashWrite("Call stack:\n");
    for (int frame = 0; frame < 64 && context.Rip; ++frame)
    {
        char where[MAX_PATH + 32], line[MAX_PATH + 64];
        Location(context.Rip, where, sizeof where);
        std::snprintf(line, sizeof line, "  #%02d %s\n", frame, where);
        CrashWrite(line);
        DWORD64 image = 0;
        if (const auto* function = RtlLookupFunctionEntry(context.Rip, &image, nullptr))
        {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, const_cast<PRUNTIME_FUNCTION>(function), &context,
                             &handler_data, &establisher, nullptr);
        }
        else
        {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
    }
}

void WriteDump(EXCEPTION_POINTERS* info)
{
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    const auto dump = dbghelp ? reinterpret_cast<DumpFn>(
        reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))) : nullptr;
    if (!dump || g_log.dump.empty()) return;
    const HANDLE file = CreateFileW(g_log.dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    const auto type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory
                                    | MiniDumpWithUnloadedModules);
    if (dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, info ? &exception : nullptr, nullptr, nullptr))
        CrashWrite(("Minidump: " + Utf8(g_log.dump) + "\n").c_str());
    CloseHandle(file);
}

// Double-clicked games get a console of their own that closes with them;
// one started from a terminal shares it, and the terminal keeps the text.
bool OwnConsole()
{
    DWORD processes[2];
    return GetConsoleProcessList(processes, 2) <= 1 && !std::getenv("MSCHARGED_NO_DIALOGS");
}

void CrashDialog(const char* headline)
{
    if (!OwnConsole()) return;
    std::wstring text = L"Mario Strikers Charged ";
    wchar_t wide[512];
    MultiByteToWideChar(CP_UTF8, 0, headline, -1, wide, 512);
    text += wide;
    text += L"\n\nA report was saved to:\n" + g_log.path.wstring()
        + L"\n\nPlease attach mscharged.log (and mscharged-crash.dmp, if present) to your bug report.";
    MessageBoxW(nullptr, text.c_str(), L"Mario Strikers Charged", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
}

const char* ExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "page could not be read";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case 0x20474343: return "uncaught C++ exception";
    case 0xE06D7363: return "uncaught C++ exception (MSVC)";
    case 0xC0000409: return "stack buffer overrun";
    case 0xC0000374: return "heap corruption";
    default: return "exception";
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    if (g_crashing.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    std::fflush(nullptr);
    Drain();
    const auto& record = *info->ExceptionRecord;
    char where[MAX_PATH + 32], line[MAX_PATH + 160];
    Location(DWORD64(record.ExceptionAddress), where, sizeof where);
    std::snprintf(line, sizeof line, "\n*** Crash: %s (0x%08lx) at %s (thread %lu)\n",
                  ExceptionName(record.ExceptionCode), record.ExceptionCode, where, GetCurrentThreadId());
    CrashWrite(line);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
    {
        const auto kind = record.ExceptionInformation[0];
        std::snprintf(line, sizeof line, "Access violation %s address 0x%llx\n",
                      kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                      (unsigned long long)record.ExceptionInformation[1]);
        CrashWrite(line);
    }
    WriteStack(*info->ContextRecord);
    WriteDump(info);
    CrashDialog("crashed.");
    return EXCEPTION_EXECUTE_HANDLER;
}

void AbortHandler(int)
{
    if (g_crashing.exchange(true)) return;
    std::fflush(nullptr);
    Drain();
    CrashWrite("\n*** Crash: the program aborted\n");
    CONTEXT context{};
    RtlCaptureContext(&context);
    WriteStack(context);
    WriteDump(nullptr);
    CrashDialog("stopped unexpectedly.");
}

void InstallCrashHandlers()
{
    SetUnhandledExceptionFilter(CrashFilter);
    std::signal(SIGABRT, AbortHandler);
    // No Windows error-reporting or abort() dialog after ours.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#else
void WriteNumber(char* out, std::size_t size, const char* format, unsigned long long value)
{
    std::snprintf(out, size, format, value);
}

void SignalHandler(int number, siginfo_t* info, void*)
{
    if (!g_crashing.exchange(true))
    {
        Drain();
        char line[256];
        std::snprintf(line, sizeof line, "\n*** Crash: signal %d (%s), address %p\n", number, strsignal(number),
                      info ? info->si_addr : nullptr);
        CrashWrite(line);
#ifdef MSCHARGED_HAS_BACKTRACE
        void* frames[64];
        const int count = backtrace(frames, 64);
        CrashWrite("Call stack:\n");
        for (int n = 0; n < count; ++n)
        {
            Dl_info module{};
            const char* name = "?";
            unsigned long long offset = (unsigned long long)frames[n];
            if (dladdr(frames[n], &module) && module.dli_fname)
            {
                name = std::strrchr(module.dli_fname, '/') ? std::strrchr(module.dli_fname, '/') + 1 : module.dli_fname;
                offset = (unsigned long long)((char*)frames[n] - (char*)module.dli_fbase);
            }
            std::snprintf(line, sizeof line, "  #%02d %s+0x%llx %s\n", n, name, offset,
                          module.dli_sname ? module.dli_sname : "");
            CrashWrite(line);
        }
#endif
        CrashWrite(("A report was saved to " + Utf8(g_log.path) + "\n").c_str());
    }
    // SA_RESETHAND restored the default action: end as the signal would.
    raise(number);
}

void InstallCrashHandlers()
{
    static char alternate[64 * 1024];
    stack_t stack{};
    stack.ss_sp = alternate;
    stack.ss_size = sizeof alternate;
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (const int number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(number, &action, nullptr);
}
#endif

void Terminate()
{
    // An exception nobody caught: name it before the abort handler logs the stack.
    std::string text = "\n*** Uncaught exception";
    if (const auto current = std::current_exception())
    {
        try { std::rethrow_exception(current); }
        catch (const std::exception& error) { text += std::string(": ") + error.what(); }
        catch (...) { text += " (not a std::exception)"; }
    }
    text += "\n";
    std::fputs(text.c_str(), stderr);
    std::fflush(nullptr);
    std::abort();
}

void SDLCALL SdlLog(void*, int category, SDL_LogPriority priority, const char* message)
{
    static const char* const names[] = {"", "trace", "verbose", "debug", "info", "warning", "error", "critical"};
    std::fprintf(stderr, "[%s] [SDL %d] %s\n", priority < SDL_LOG_PRIORITY_COUNT ? names[priority] : "log", category,
                 message);
}

fs::path ChooseDirectory()
{
    if (const char* chosen = std::getenv("MSCHARGED_LOG_DIR"); chosen && *chosen)
        return std::string(chosen) == "off" ? fs::path() : PathFromUtf8(chosen);
    std::error_code error;
    if (const char* base = SDL_GetBasePath())
    {
        const fs::path directory = PathFromUtf8(base) / "logs";
        fs::create_directories(directory, error);
        const auto probe = directory / ".write-test";
        if (!error && std::ofstream(probe).good())
        {
            fs::remove(probe, error);
            return directory;
        }
    }
    if (char* pref = SDL_GetPrefPath("mscharged", "mscharged"))
    {
        const fs::path directory = PathFromUtf8(pref) / "logs";
        SDL_free(pref);
        return directory;
    }
    return {};
}
} // namespace

void StartSessionLog(const char* version, int argc, char** argv)
{
    if (g_log.started) return;
    g_log.started = true;
    const fs::path directory = ChooseDirectory();
    if (directory.empty()) return;
    std::error_code error;
    fs::create_directories(directory, error);
    g_log.path = directory / "mscharged.log";
    g_log.dump = directory / "mscharged-crash.dmp";
    if (fs::exists(g_log.path, error)) fs::rename(g_log.path, directory / "mscharged.previous.log", error);
    fs::remove(g_log.dump, error);
#ifdef _WIN32
    g_log.file = _wopen(g_log.path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
#else
    g_log.file = ::open(g_log.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
    if (g_log.file < 0)
    {
        std::fprintf(stderr, "Cannot write the log %s\n", Utf8(g_log.path).c_str());
        g_log.path.clear();
        return;
    }
    Header(version, argc, argv);
    std::fflush(nullptr);
    Redirect(1, g_log.out);
    Redirect(2, g_log.err);
#ifdef _WIN32
    // The C runtime has no line buffering; unbuffered keeps lines in order.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif
    SDL_SetLogOutputFunction(SdlLog, nullptr);
    std::set_terminate(Terminate);
    InstallCrashHandlers();
    std::atexit([] {
        std::fflush(nullptr);
        Drain();
        WriteLog("\n=== Session ended ===\n", 23);
    });
}

fs::path SessionLogPath() { return g_log.file >= 0 ? g_log.path : fs::path(); }

void SessionLogLine(const std::string& text)
{
    const std::string line = text + "\n";
    std::fflush(nullptr);
    Drain();
    WriteLog(line.data(), line.size());
}

void LogSessionDisplays(SDL_Window* window)
{
    if (g_log.file < 0) return;
    std::string text = Line("Video driver: %s\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int n = 0; displays && n < count; ++n)
    {
        SDL_Rect bounds{}, usable{};
        SDL_GetDisplayBounds(displays[n], &bounds);
        SDL_GetDisplayUsableBounds(displays[n], &usable);
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displays[n]);
        const char* name = SDL_GetDisplayName(displays[n]);
        text += Line("Display %d%s: %s, %dx%d at %d,%d (usable %dx%d), mode %dx%d @ %.2f Hz, density %.2f, "
                     "content scale %.2f\n",
                     n + 1, displays[n] == primary ? " (primary)" : "", name ? name : "?", bounds.w, bounds.h,
                     bounds.x, bounds.y, usable.w, usable.h, mode ? mode->w : 0, mode ? mode->h : 0,
                     mode ? double(mode->refresh_rate) : 0.0, mode ? double(mode->pixel_density) : 0.0,
                     double(SDL_GetDisplayContentScale(displays[n])));
    }
    SDL_free(displays);
    if (window)
    {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(window, &w, &h);
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        const auto flags = SDL_GetWindowFlags(window);
        int display = 0;
        if (const SDL_DisplayID id = SDL_GetDisplayForWindow(window))
        {
            SDL_DisplayID* all = SDL_GetDisplays(&count);
            for (int n = 0; all && n < count; ++n)
                if (all[n] == id) display = n + 1;
            SDL_free(all);
        }
        text += Line("Window: %dx%d (%dx%d pixels) on display %d, %s\n", w, h, pw, ph, display,
                     flags & SDL_WINDOW_FULLSCREEN ? "fullscreen" : "windowed");
    }
    SessionLogLine(text.substr(0, text.size() - 1));
}

void FlushSessionLog()
{
    std::fflush(nullptr);
    Drain();
}

void ReportFatalError(const std::string& message)
{
    std::fprintf(stderr, "%s\n", message.c_str());
    FlushSessionLog();
#ifdef _WIN32
    if (!OwnConsole()) return;
    const int size = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
    std::wstring wide(std::size_t(size > 0 ? size : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wide.data(), size);
    wide.resize(wcslen(wide.c_str()));
    std::wstring text = L"The game stopped with an error:\n\n" + wide;
    if (g_log.file >= 0)
        text += L"\n\nDetails are in:\n" + g_log.path.wstring() + L"\n\nPlease attach that file to your bug report.";
    MessageBoxW(nullptr, text.c_str(), L"Mario Strikers Charged", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
#endif
}
} // namespace mscharged::platform
