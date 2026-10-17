#include "platform/macos_app.h"

#include <SDL3/SDL_messagebox.h>

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#include <cerrno>
#include <climits>
#include <filesystem>
#include <string>
#include <system_error>

extern char** environ;

namespace fs = std::filesystem;

namespace mscharged::platform
{
namespace
{
constexpr const char* kQuarantine = "com.apple.quarantine";

// Security.framework has these since macOS 10.12; its header is not public.
using IsTranslocatedUrl = Boolean (*)(CFURLRef, bool*, CFErrorRef*);
using CreateOriginalPathForUrl = CFURLRef (*)(CFURLRef, CFErrorRef*);

std::string PathOf(CFURLRef url)
{
    char buffer[PATH_MAX];
    if (!url || !CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(buffer), sizeof buffer)) return {};
    return buffer;
}

// What `xattr -dr com.apple.quarantine FOLDER` does.
bool ClearQuarantine(const fs::path& folder)
{
    std::error_code error;
    removexattr(folder.c_str(), kQuarantine, XATTR_NOFOLLOW);
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        removexattr(it->path().c_str(), kQuarantine, XATTR_NOFOLLOW);
    return !error;
}

bool Quarantined(const fs::path& path)
{
    return getxattr(path.c_str(), kQuarantine, nullptr, 0, 0, XATTR_NOFOLLOW) >= 0 || errno != ENOATTR;
}

bool OpenNewInstance(const std::string& app)
{
    char* arguments[] = {const_cast<char*>("open"), const_cast<char*>("-n"), const_cast<char*>(app.c_str()), nullptr};
    pid_t opener = 0;
    if (posix_spawn(&opener, "/usr/bin/open", nullptr, nullptr, arguments, environ) != 0) return false;
    int status = 0;
    while (waitpid(opener, &status, 0) < 0)
        if (errno != EINTR) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// The button chosen: 1 = fix, 0 = quit.
int Ask(const std::string& message, bool offer_fix)
{
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Allow and restart"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | (offer_fix ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT), 0, "Quit"}};
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_WARNING;
    data.title = "Recharged";
    data.message = message.c_str();
    data.numbuttons = offer_fix ? 2 : 1;
    data.buttons = offer_fix ? buttons : buttons + 1;
    int chosen = 0;
    return SDL_ShowMessageBox(&data, &chosen) ? chosen : 0;
}
} // namespace

bool LeaveAppTranslocation(int& code)
{
    const CFBundleRef bundle = CFBundleGetMainBundle();
    const CFURLRef app = bundle ? CFBundleCopyBundleURL(bundle) : nullptr;
    if (!app) return false;
    void* security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY | RTLD_LOCAL);
    const auto is_translocated
        = security ? reinterpret_cast<IsTranslocatedUrl>(dlsym(security, "SecTranslocateIsTranslocatedURL")) : nullptr;
    const auto original_path = security
        ? reinterpret_cast<CreateOriginalPathForUrl>(dlsym(security, "SecTranslocateCreateOriginalPathForURL"))
        : nullptr;
    bool translocated = false;
    if (!is_translocated || !is_translocated(app, &translocated, nullptr))
        translocated = PathOf(app).find("/AppTranslocation/") != std::string::npos;
    std::string original;
    if (translocated && original_path)
    {
        const CFURLRef url = original_path(app, nullptr);
        original = PathOf(url);
        if (url) CFRelease(url);
    }
    CFRelease(app);
    if (!translocated) return false;

    code = 1;
    const std::string reason
        = "macOS started Recharged from a temporary, read-only copy, because its folder came from the internet and is "
          "still marked as quarantined. From there Recharged cannot see its game files, settings and saves.\n\n";
    if (original.empty())
    {
        Ask(reason + "Remove that mark from the folder you unpacked Recharged into, then start it again. In Terminal:\n\n"
                     "xattr -dr com.apple.quarantine <folder>",
            false);
        return true;
    }
    const fs::path folder = fs::path(original).parent_path();
    const std::string command = "xattr -dr com.apple.quarantine \"" + folder.string() + "\"";
    if (Ask(reason + "Recharged can remove that mark from its folder and start again:\n" + folder.string()
                + "\n\nTo do it yourself instead, run in Terminal:\n" + command,
            true) != 1)
        return true;
    if (!ClearQuarantine(folder) || Quarantined(original))
    {
        Ask("Recharged could not change its folder. Run this in Terminal, then start Recharged again:\n\n" + command,
            false);
        return true;
    }
    if (!OpenNewInstance(original))
    {
        Ask("The folder is ready. Please start Recharged again.", false);
        return true;
    }
    code = 0;
    return true;
}
} // namespace mscharged::platform
