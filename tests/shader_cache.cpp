#include "platform/shader_cache.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using mscharged::platform::ClearShaderCache;
using mscharged::platform::ShaderCacheUse;
unsigned checks = 0;
constexpr std::array<const char*, 8> names = {
    "pipeline_cache.db", "pipeline_cache.db-wal", "pipeline_cache.db-shm", "pipeline_cache.db-journal",
    "dawn_cache.db", "dawn_cache.db-wal", "dawn_cache.db-shm", "dawn_cache.db-journal"};

void Check(bool yes, const char* message) {
    ++checks;
    if (!yes) throw std::runtime_error(message);
}
void Write(const fs::path& path, const std::string& bytes = "cache bytes") {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    Check(bool(stream), "Fixture file could not be written");
}
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
template<class Operation> void Reject(Operation operation, const char* message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}

int Hold(const fs::path& directory) {
    try {
        ShaderCacheUse lease(directory);
        Write(directory / "child-ready");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!fs::exists(directory / "child-release")) {
            if (std::chrono::steady_clock::now() >= deadline) return 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;
    } catch (...) { return 3; }
}

struct Child {
#ifdef _WIN32
    HANDLE process = nullptr;
    explicit Child(const fs::path& directory) {
        std::array<wchar_t, 32768> executable{};
        const auto size = GetModuleFileNameW(nullptr, executable.data(), executable.size());
        Check(size > 0 && size < executable.size(), "Cannot resolve test executable");
        // These paths are authored by this fixture and contain no quote. The
        // directory has a nonseparator final component, so quoting is literal.
        const std::wstring path(executable.data(), size);
        const std::wstring folder = directory.native();
        Check(path.find(L'"') == std::wstring::npos && folder.find(L'"') == std::wstring::npos,
              "Unsupported quote in fixture path");
        auto command = L"\"" + path + L"\" --hold \"" + folder + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        Check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                             nullptr, nullptr, &startup, &info), "Cannot launch cache lease child");
        process = info.hProcess;
        CloseHandle(info.hThread);
    }
    ~Child() { if (process) { TerminateProcess(process, 99); WaitForSingleObject(process, 5000); CloseHandle(process); } }
    void Crash() {
        Check(TerminateProcess(process, 99), "Could not terminate owned lease child");
        Check(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "Owned lease child survived termination");
        CloseHandle(process); process = nullptr;
    }
#else
    pid_t process = -1;
    explicit Child(const fs::path& directory) {
        process = fork();
        Check(process >= 0, "Cannot launch cache lease child");
        if (process == 0) _exit(Hold(directory));
    }
    ~Child() { if (process > 0) { kill(process, SIGKILL); waitpid(process, nullptr, 0); } }
    void Crash() {
        Check(kill(process, SIGKILL) == 0, "Could not terminate owned lease child");
        Check(waitpid(process, nullptr, 0) == process, "Owned lease child survived termination");
        process = -1;
    }
#endif
};

void Run(const fs::path& root) {
    Check(mscharged::platform::ShaderCacheDirectory(root) == root / "data",
          "Runtime and launcher cache directory disagree");
    const auto missing = root / "missing";
    Check(ClearShaderCache(missing).removed_files == 0 && !fs::exists(missing),
          "Clearing a missing cache created or removed data");
    const auto directory = root / "cache";
    for (const auto* name : names) Write(directory / name);
    Write(directory / "nand/title/game-save", "saved match");
    Write(directory / "logs/recharged.log", "session log");
    Write(directory / "recharged.ini", "settings");
    Write(directory / "pipeline_cache.db.backup", "keep backup");
    {
        ShaderCacheUse game(directory);
        ShaderCacheUse other_game(directory);
        Reject([&] { ClearShaderCache(directory); }, "A live shared game lease allowed cache deletion");
        for (const auto* name : names) Check(Read(directory / name) == "cache bytes", "Busy clear modified cache data");
        auto moved = std::move(other_game);
        Reject([&] { ClearShaderCache(directory); }, "Moving a game lease released it");
    }
    const auto cleared = ClearShaderCache(directory);
    Check(cleared.removed_files == 8 && cleared.removed_bytes == 8 * std::string("cache bytes").size(),
          "Cache clear did not report all database and SQLite sidecar bytes");
    for (const auto* name : names) Check(!fs::exists(directory / name), "A selected cache file survived clearing");
    Check(Read(directory / "nand/title/game-save") == "saved match" &&
          Read(directory / "logs/recharged.log") == "session log" &&
          Read(directory / "recharged.ini") == "settings" &&
          Read(directory / "pipeline_cache.db.backup") == "keep backup",
          "Cache clearing touched a save, log, configuration or unrelated file");
    Check(ClearShaderCache(directory).removed_files == 0, "Repeated empty clearing did not succeed");

    // Preflight rejects the whole set before any earlier-listed good file is
    // removed. A directory under a database name must never be traversed.
    Write(directory / names[0]);
    Write(directory / names[4] / "keep", "directory payload");
    Reject([&] { ClearShaderCache(directory); }, "Cache-named directory was accepted");
    Check(Read(directory / names[0]) == "cache bytes" && Read(directory / names[4] / "keep") == "directory payload",
          "Unsafe-target preflight partially cleared data");
    fs::remove(directory / names[4] / "keep"); fs::remove(directory / names[4]);
    Write(root / "outside", "outside target");
    std::error_code symlink_error;
    fs::create_symlink(root / "outside", directory / names[4], symlink_error);
    if (!symlink_error) {
        Reject([&] { ClearShaderCache(directory); }, "Symlink cache target was accepted");
        Check(Read(directory / names[0]) == "cache bytes" && Read(root / "outside") == "outside target",
              "Symlink preflight modified an unrelated target");
        fs::remove(directory / names[4]);
        const auto linked = root / "linked-cache";
        fs::create_directory_symlink(directory, linked);
        Reject([&] { ClearShaderCache(linked); }, "Symlink cache directory was accepted");
        fs::remove(linked);
        const auto poisoned = root / "poisoned-lock";
        fs::create_directory(poisoned);
        fs::create_symlink(root / "outside", poisoned / ".graphics-cache.lock");
        Reject([&] { ShaderCacheUse lease(poisoned); }, "Symlink cache lock was accepted");
        Reject([&] { ClearShaderCache(poisoned); }, "Clear accepted a symlink cache lock");
        Check(Read(root / "outside") == "outside target", "Opening the lock modified its symlink target");
    } else {
#ifndef _WIN32
        Check(false, "POSIX symlink fixture setup failed");
#else
        std::cout << "Windows symlink checks unavailable without symlink privilege\n";
#endif
    }
    ClearShaderCache(directory);

    Write(directory / names[0]);
    Child child(directory);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!fs::exists(directory / "child-ready")) {
        Check(std::chrono::steady_clock::now() < deadline, "Cache lease child did not become ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Reject([&] { ClearShaderCache(directory); }, "A separate live game process allowed cache deletion");
    Check(Read(directory / names[0]) == "cache bytes", "Cross-process busy clear modified cache data");
    child.Crash();
    Check(ClearShaderCache(directory).removed_files == 1, "Crashed process retained a stale cache lock");
    { ShaderCacheUse next_game(directory); }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--hold") return Hold(fs::path(argv[2]));
    const auto root = fs::temp_directory_path() /
        ("charged-shader-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        Check(!fs::exists(root), "Fixture root already exists");
        Run(root);
        fs::remove_all(root);
        std::cout << "Shader cache checks passed: " << checks << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Shader cache test failed: " << error.what() << "; evidence: " << root << '\n';
        return 1;
    }
}
