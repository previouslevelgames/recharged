#include "platform/shader_cache.h"
#include "bootstrap/config.h"

#include <array>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mscharged::platform {
namespace {
constexpr const char* kLockName = ".graphics-cache.lock";
constexpr std::array<const char*, 8> kCacheFiles = {
    "pipeline_cache.db", "pipeline_cache.db-wal", "pipeline_cache.db-shm", "pipeline_cache.db-journal",
    "dawn_cache.db", "dawn_cache.db-wal", "dawn_cache.db-shm", "dawn_cache.db-journal"};

[[noreturn]] void Busy() {
    throw std::runtime_error("The shader cache is in use. Close the game and try again.");
}

[[noreturn]] void Unsafe() {
    throw std::runtime_error("The shader cache contains a link or an unexpected file type; clearing was stopped.");
}

[[noreturn]] void Failure(const char* operation, int error) {
    throw std::system_error(error, std::system_category(), operation);
}

#ifdef _WIN32
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&&) = delete;
    Handle(const Handle&) = delete;
};

BY_HANDLE_FILE_INFORMATION Info(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) Failure("Inspect shader cache", GetLastError());
    return info;
}

struct Lease {
    Handle folder;
    Handle lock;

    Lease(const std::filesystem::path& path, bool exclusive)
        : folder(OpenFolder(path)),
          lock(OpenLock(path)) {
        const auto lock_info = Info(lock.value);
        if ((lock_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            lock_info.nNumberOfLinks != 1) Unsafe();
        OVERLAPPED region{};
        const auto flags = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
        if (!LockFileEx(lock.value, flags, 0, 1, 0, &region)) {
            const auto error = GetLastError();
            if (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION) Busy();
            Failure("Lock shader cache", error);
        }
    }

    static HANDLE OpenFolder(const std::filesystem::path& path) {
        Handle folder(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (folder.value == INVALID_HANDLE_VALUE) Failure("Open shader cache folder", GetLastError());
        const auto info = Info(folder.value);
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Unsafe();
        return std::exchange(folder.value, INVALID_HANDLE_VALUE);
    }

    static HANDLE OpenLock(const std::filesystem::path& path) {
        const auto file = path / kLockName;
        const auto result = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (result == INVALID_HANDLE_VALUE) Failure("Open shader cache lock", GetLastError());
        return result;
    }
};
#else
struct Handle {
    int value = -1;
    explicit Handle(int handle = -1) : value(handle) {}
    ~Handle() { if (value >= 0) close(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Handle& operator=(Handle&&) = delete;
    Handle(const Handle&) = delete;
};

struct Lease {
    Handle folder;
    Handle lock;
    Lease(const std::filesystem::path& path, bool exclusive)
        : folder(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)),
          lock(OpenLock(folder.value)) {
        struct stat info{};
        if (fstat(lock.value, &info) != 0) Failure("Inspect shader cache lock", errno);
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1) Unsafe();
        if (flock(lock.value, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
            const auto error = errno;
            if (error == EWOULDBLOCK || error == EAGAIN) Busy();
            Failure("Lock shader cache", error);
        }
    }

    static int OpenLock(int directory) {
        if (directory < 0) Failure("Open shader cache folder", errno);
        const auto result = openat(directory, kLockName, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (result < 0) Failure("Open shader cache lock", errno);
        return result;
    }
};
#endif

bool ExistingDirectory(const std::filesystem::path& path) {
    if (path.empty()) throw std::runtime_error("The shader cache folder is empty.");
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found)
        return false;
    if (error) throw std::system_error(error, "Inspect shader cache folder");
    if (!std::filesystem::is_directory(status)) Unsafe();
    return true;
}
} // namespace

struct ShaderCacheUse::State {
    Lease lease;
    explicit State(const std::filesystem::path& directory) : lease(directory, false) {}
};

std::filesystem::path ShaderCacheDirectory(const std::filesystem::path& executable_directory) {
    return DataFolder(executable_directory);
}

ShaderCacheUse::ShaderCacheUse(const std::filesystem::path& directory) {
    if (!ExistingDirectory(directory)) std::filesystem::create_directories(directory);
    state_ = std::make_unique<State>(directory);
}
ShaderCacheUse::~ShaderCacheUse() = default;
ShaderCacheUse::ShaderCacheUse(ShaderCacheUse&&) noexcept = default;
ShaderCacheUse& ShaderCacheUse::operator=(ShaderCacheUse&&) noexcept = default;

ShaderCacheClearResult ClearShaderCache(const std::filesystem::path& directory) {
    ShaderCacheClearResult result;
    if (!ExistingDirectory(directory)) return result;
    Lease lease(directory, true);
#ifdef _WIN32
    // Denying delete sharing pins each inspected file until deletion by its own
    // handle; a path replacement cannot redirect the subsequent operation.
    std::vector<Handle> files;
    for (const auto* name : kCacheFiles) {
        const auto file = directory / name;
        Handle handle(CreateFileW(file.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) continue;
            if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) Busy();
            Failure("Open shader cache for clearing", error);
        }
        const auto info = Info(handle.value);
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nNumberOfLinks != 1) Unsafe();
        result.removed_bytes += (std::uintmax_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        files.push_back(std::move(handle));
    }
    for (const auto& file : files) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition)))
            Failure("Clear shader cache file", GetLastError());
        ++result.removed_files;
    }
#else
    struct File { const char* name; dev_t device; ino_t inode; };
    std::vector<File> files;
    for (const auto* name : kCacheFiles) {
        struct stat info{};
        if (fstatat(lease.folder.value, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            Failure("Inspect shader cache file", errno);
        }
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1) Unsafe();
        result.removed_bytes += info.st_size;
        files.push_back({name, info.st_dev, info.st_ino});
    }
    // Preflight every name before deleting any. The shared lease excludes all
    // current game writers; relative operations keep renamed parents harmless.
    for (const auto& file : files) {
        struct stat info{};
        if (fstatat(lease.folder.value, file.name, &info, AT_SYMLINK_NOFOLLOW) != 0)
            Failure("Recheck shader cache file", errno);
        if (!S_ISREG(info.st_mode) || info.st_dev != file.device || info.st_ino != file.inode || info.st_nlink != 1)
            Unsafe();
        if (unlinkat(lease.folder.value, file.name, 0) != 0) Failure("Clear shader cache file", errno);
        ++result.removed_files;
    }
#endif
    return result;
}
} // namespace mscharged::platform
