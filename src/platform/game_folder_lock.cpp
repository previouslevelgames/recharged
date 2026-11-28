#include "platform/game_folder_lock.h"

#include "platform/path.h"

#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mscharged::platform {
namespace {
constexpr const char* kLockName = ".game.lock";
constexpr const char* kWatchers = "watching";
} // namespace

bool LockGameFolder(const std::filesystem::path& data_folder)
{
    const auto file = data_folder / kLockName;
#ifdef _WIN32
    // Not inheritable: a process the game starts holds no lock.
    const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return true;
    OVERLAPPED region{};
    if (LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region)) return true;
    const auto error = GetLastError();
    CloseHandle(lock);
    return error != ERROR_LOCK_VIOLATION && error != ERROR_SHARING_VIOLATION;
#elif defined(__SWITCH__)
    // The console runs one program at a time.
    (void)file;
    return true;
#else
    const int lock = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) return true;
    if (flock(lock, LOCK_EX | LOCK_NB) == 0) return true;
    const int error = errno;
    close(lock);
    return error != EWOULDBLOCK && error != EAGAIN;
#endif
}

bool GameFolderInUse(const std::filesystem::path& data_folder)
{
    const auto file = data_folder / kLockName;
#ifdef _WIN32
    const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED region{};
    bool in_use = false;
    if (LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region))
        UnlockFileEx(lock, 0, 1, 0, &region);
    else
    {
        const auto error = GetLastError();
        in_use = error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION;
    }
    CloseHandle(lock);
    return in_use;
#elif defined(__SWITCH__)
    (void)file;
    return false;
#else
    const int lock = open(file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lock < 0) return false;
    bool in_use = false;
    if (flock(lock, LOCK_EX | LOCK_NB) == 0) flock(lock, LOCK_UN);
    else in_use = errno == EWOULDBLOCK || errno == EAGAIN;
    close(lock);
    return in_use;
#endif
}

void RemoveEndedWatchers(const std::filesystem::path& data_folder)
{
    namespace fs = std::filesystem;
    const auto watchers = data_folder / kWatchers;
    std::vector<fs::path> ended;
    std::error_code error;
    for (fs::directory_iterator it(watchers, error), end; !error && it != end; it.increment(error))
        if (std::error_code kind; it->is_directory(kind) && !it->is_symlink(kind) && !GameFolderInUse(it->path()))
            ended.push_back(it->path());
    for (const auto& folder : ended) fs::remove_all(folder, error);
    fs::remove(watchers, error); // only when no watcher is left
}

std::filesystem::path PrepareWatcherFolder(const std::filesystem::path& data_folder,
                                           const std::filesystem::path& title_save_folder)
{
    namespace fs = std::filesystem;
    RemoveEndedWatchers(data_folder);
    const auto watchers = data_folder / kWatchers;
    try
    {
        fs::create_directories(watchers);
        for (int number = 1; number <= 16; ++number)
        {
            // One that is there belongs to a watcher that still runs.
            const auto folder = watchers / std::to_string(number);
            if (!fs::create_directory(folder) || !LockGameFolder(folder)) continue;
            // The data folder before the catalog, as the game writes them.
            const auto nand = folder / "nand";
            fs::create_directory(nand);
            if (fs::is_regular_file(title_save_folder / "metadata.txt"))
            {
                if (fs::is_directory(title_save_folder / "data"))
                    fs::copy(title_save_folder / "data", nand / "data",
                             fs::copy_options::recursive | fs::copy_options::skip_symlinks);
                fs::copy_file(title_save_folder / "metadata.txt", nand / "metadata.txt");
            }
            if (fs::is_regular_file(data_folder / "native-rtc.bin"))
                fs::copy_file(data_folder / "native-rtc.bin", folder / "native-rtc.bin");
            return folder;
        }
    }
    catch (const fs::filesystem_error& failure)
    {
        throw std::runtime_error("Cannot make a folder to watch from in " + PathUtf8(failure.path1()) + ": "
                                 + failure.code().message());
    }
    throw std::runtime_error("Too many games watch from this folder: close one of them.");
}

} // namespace mscharged::platform
