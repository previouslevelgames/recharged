#include "platform/watch_link_handler.h"

#include "platform/path.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mscharged::platform
{
namespace
{
#if defined(_WIN32)
constexpr const wchar_t* kKey = L"Software\\Classes\\recharged";
constexpr const wchar_t* kCommandKey = L"Software\\Classes\\recharged\\shell\\open\\command";
constexpr const wchar_t* kIconKey = L"Software\\Classes\\recharged\\DefaultIcon";

// A string value of HKEY_CURRENT_USER\`key` (`name` null: the default one);
// empty when there is none.
std::wstring Read(const wchar_t* key, const wchar_t* name)
{
    wchar_t value[2048];
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return {};
    return value;
}

bool Write(const wchar_t* key, const wchar_t* name, const std::wstring& value)
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value.c_str(),
                           DWORD((value.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS;
}
#elif !defined(__APPLE__) && !defined(__SWITCH__)
// $variable, or ~/fallback when it is unset or not absolute (the XDG rule).
std::filesystem::path XdgFolder(const char* variable, const char* fallback)
{
    if (const char* value = std::getenv(variable); value && value[0] == '/') return value;
    const char* home = std::getenv("HOME");
    return home && home[0] == '/' ? std::filesystem::path(home) / fallback : std::filesystem::path();
}

// A desktop entry's Exec argument: quoted, with the characters the
// specification wants escaped (a backslash takes four, as string values
// unescape once more).
std::string Quoted(const std::filesystem::path& program)
{
    std::string out = "\"";
    for (const char c : PathUtf8(program))
    {
        if (c == '\\') out += "\\\\\\\\";
        else if (c == '"' || c == '`' || c == '$') out += std::string("\\\\") + c;
        else out += c;
    }
    return out + '"';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Replaces `path` whole with `text` unless it holds it already; whether it
// was written.
bool Replace(const std::filesystem::path& path, const std::string& text)
{
    std::error_code error;
    if (std::filesystem::exists(path, error) && ReadText(path) == text) return false;
    std::filesystem::create_directories(path.parent_path(), error);
    auto partial = path;
    partial += ".tmp";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return false;
    }
    std::filesystem::rename(partial, path, error);
    if (error) std::filesystem::remove(partial, error);
    return !error;
}

// mimeapps.list with `key`=`value` in its [Default Applications] section,
// the rest kept as it was.
std::string WithDefault(const std::string& list, const std::string& key, const std::string& value)
{
    std::istringstream in(list);
    std::string out, line;
    bool in_section = false, found_section = false, done = false;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.front() == '[')
        {
            if (in_section && !done)
            {
                out += key + '=' + value + '\n';
                done = true;
            }
            in_section = line == "[Default Applications]";
            found_section = found_section || in_section;
        }
        else if (in_section && line.compare(0, key.size() + 1, key + '=') == 0)
        {
            if (!done) out += key + '=' + value + '\n';
            done = true;
            continue;
        }
        out += line + '\n';
    }
    if (in_section && !done) out += key + '=' + value + '\n';
    else if (!found_section) out += (out.empty() || out.back() == '\n' ? "" : "\n") + std::string("[Default Applications]\n") + key + '=' + value + '\n';
    return out;
}
#endif
} // namespace

std::string RegisterWatchLinks(const std::filesystem::path& program)
{
#if defined(_WIN32)
    const std::wstring exe = program.wstring();
    const std::wstring command = L"\"" + exe + L"\" \"%1\"";
    if (Read(kCommandKey, nullptr) == command) return {};
    const bool written = Write(kKey, nullptr, L"URL:Recharged watch link") && Write(kKey, L"URL Protocol", L"")
        && Write(kIconKey, nullptr, L"\"" + exe + L"\",0") && Write(kCommandKey, nullptr, command);
    return written ? "Watch links: recharged:// opens " + PathUtf8(program)
                   : "Watch links: the registry did not take recharged://";
#elif defined(__APPLE__) || defined(__SWITCH__)
    (void)program;
    return {};
#else
    const auto applications = XdgFolder("XDG_DATA_HOME", ".local/share");
    const auto config = XdgFolder("XDG_CONFIG_HOME", ".config");
    if (applications.empty() || config.empty()) return {};
    std::string entry = "[Desktop Entry]\nType=Application\nName=Recharged\n"
                        "Comment=Watches an online match from the WFC server's dashboard\nExec="
        + Quoted(program) + " %u\nTerminal=false\nNoDisplay=true\nMimeType=x-scheme-handler/recharged;\n";
    const auto icon = program.parent_path() / "assets" / "launcher" / "icon.png";
    std::error_code error;
    if (std::filesystem::exists(icon, error)) entry += "Icon=" + PathUtf8(icon) + '\n';
    bool changed = Replace(applications / "applications" / "recharged-watch.desktop", entry);
    const auto mimeapps = config / "mimeapps.list";
    const std::string list = ReadText(mimeapps);
    const std::string updated = WithDefault(list, "x-scheme-handler/recharged", "recharged-watch.desktop");
    if (updated != list) changed = Replace(mimeapps, updated) || changed;
    return changed ? "Watch links: recharged:// opens " + PathUtf8(program) : std::string();
#endif
}
} // namespace mscharged::platform
