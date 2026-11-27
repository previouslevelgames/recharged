#include "platform/game_folder_lock.h"

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

} // namespace mscharged::platform
