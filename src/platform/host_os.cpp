#include "platform/host_os.h"

#include <cstdio>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

namespace mscharged::platform {
namespace {
// The program's own architecture: the slice that runs.
constexpr const char* kArchitecture =
#if defined(__x86_64__) || defined(_M_X64)
    "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    "x86";
#else
    "unknown CPU";
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
// The distribution's name from os-release (PRETTY_NAME), at most 40
// characters so the architecture still fits; empty when there is none.
std::string Distribution()
{
    for (const char* path : {"/etc/os-release", "/usr/lib/os-release"})
    {
        std::ifstream file(path);
        for (std::string line; std::getline(file, line);)
        {
            if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
            std::string_view value = std::string_view(line).substr(12);
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
                value = value.substr(1, value.size() - 2);
            std::string name;
            for (const char c : value)
                if (c != '\\' && name.size() < 40) name += c;
            if (!name.empty()) return name;
        }
    }
    return {};
}
#endif
} // namespace

std::string HostOsName()
{
    char text[128] = "";
#ifdef _WIN32
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof os;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (const auto get = reinterpret_cast<RtlGetVersionFn>(
            ntdll ? reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr))
        get(&os);
    char name[32];
    if (os.dwMajorVersion == 10) std::snprintf(name, sizeof name, "Windows %d", os.dwBuildNumber >= 22000 ? 11 : 10);
    else if (os.dwMajorVersion == 6 && os.dwMinorVersion == 3) std::snprintf(name, sizeof name, "Windows 8.1");
    else if (os.dwMajorVersion == 6 && os.dwMinorVersion == 2) std::snprintf(name, sizeof name, "Windows 8");
    else if (os.dwMajorVersion == 6 && os.dwMinorVersion == 1) std::snprintf(name, sizeof name, "Windows 7");
    else std::snprintf(name, sizeof name, "Windows %lu.%lu", os.dwMajorVersion, os.dwMinorVersion);
    std::string out = name;
    std::snprintf(text, sizeof text, " (%lu), %s", os.dwBuildNumber, kArchitecture);
    out += text;
    // An x64 program on an Arm PC runs emulated (IsWow64Process2 names no
    // WOW64 machine for it, only the PC's own).
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    USHORT process = 0, machine = 0;
    if (const auto wow = reinterpret_cast<IsWow64Process2Fn>(
            kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, "IsWow64Process2")) : nullptr);
        wow && wow(GetCurrentProcess(), &process, &machine) && machine == IMAGE_FILE_MACHINE_ARM64
        && std::string_view(kArchitecture) != "arm64")
        out += " on arm64";
    // Wine (and Proton) say what they are and what they run on.
    using WineVersionFn = const char*(CDECL*)();
    using WineHostFn = void(CDECL*)(const char**, const char**);
    if (const auto wine = reinterpret_cast<WineVersionFn>(
            ntdll ? reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")) : nullptr))
    {
        const char* version = wine();
        const char* system = nullptr;
        const char* release = nullptr;
        if (const auto host = reinterpret_cast<WineHostFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_host_version"))))
            host(&system, &release);
        std::snprintf(text, sizeof text, ", Wine %.16s%s%.16s", version ? version : "", system ? " on " : "",
                      system ? system : "");
        out += text;
    }
    return out;
#elif defined(__APPLE__)
    char product[32] = "";
    std::size_t size = sizeof product;
    if (sysctlbyname("kern.osproductversion", product, &size, nullptr, 0) != 0) product[0] = '\0';
    int translated = 0;
    size = sizeof translated;
    const bool rosetta = sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 && translated == 1;
    std::snprintf(text, sizeof text, "macOS%s%s, %s%s", product[0] ? " " : "", product, kArchitecture,
                  rosetta ? " (Rosetta)" : "");
    return text;
#elif defined(__SWITCH__)
    std::snprintf(text, sizeof text, "Nintendo Switch, %s", kArchitecture);
    return text;
#else
    utsname host{};
    const bool known = uname(&host) == 0;
    const std::string system = known && host.sysname[0] ? host.sysname : "Unix";
    const std::string distribution = Distribution();
    if (!distribution.empty())
        std::snprintf(text, sizeof text, "%s (%s), %s", system.c_str(), distribution.c_str(), kArchitecture);
    else
        std::snprintf(text, sizeof text, "%s %.24s, %s", system.c_str(), known ? host.release : "", kArchitecture);
    return text;
#endif
}

std::string HostPlatform()
{
    if (std::string_view(kArchitecture) == "unknown CPU") return {};
#ifdef _WIN32
    std::string system = "windows";
#elif defined(__APPLE__)
    std::string system = "macos";
#elif defined(__SWITCH__)
    std::string system = "switch";
#else
    // uname's, lowercase: "linux", "freebsd".
    std::string system;
    if (utsname host{}; uname(&host) == 0)
    {
        for (const char* c = host.sysname; *c; ++c)
        {
            if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) system += *c;
            else if (*c >= 'A' && *c <= 'Z') system += char(*c - 'A' + 'a');
        }
    }
    if (system.empty()) return {};
#endif
    return system + ' ' + kArchitecture;
}
} // namespace mscharged::platform
