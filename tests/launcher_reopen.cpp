// [launcher] reopen_after_game on Linux and macOS: the game's process ends its
// session log and becomes the launcher (execv). The new image must get the
// arguments, none of the game's descriptors, no blocked signals and a working
// stdout, and its own log must keep the game's as recharged.previous.log.
#include "platform/launcher_reopen.h"
#include "platform/session_log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::stringstream text;
    text << input.rdbuf();
    return text.str();
}

// The launcher's side, in the replaced process image.
int Reopened(int argc, char** argv)
{
    // Checked first: the new log's own descriptors may take the freed number.
    const int fd = std::atoi(argv[2]);
    const bool closed = fcntl(fd, F_GETFD) == -1 && errno == EBADF;
    sigset_t mask;
    pthread_sigmask(SIG_SETMASK, nullptr, &mask);
    const bool unblocked = !sigismember(&mask, SIGTERM) && !sigismember(&mask, SIGUSR1);
    mscharged::platform::StartSessionLog("test", argc, argv);
    std::printf("reopened output\n"); // a dead pipe from the game would end it with SIGPIPE
    if (!closed) std::fprintf(stderr, "the game's descriptor %d is still open\n", fd);
    if (!unblocked) std::fprintf(stderr, "a signal is still blocked\n");
    mscharged::platform::FlushSessionLog();
    return closed && unblocked ? 0 : 1;
}

// The game's side: a session log, a descriptor without close-on-exec and
// blocked signals, as a game's threads leave them.
[[noreturn]] void Game(const std::filesystem::path& directory, char* program)
{
    setenv("RECHARGED_LOG_DIR", directory.c_str(), 1);
    char* start[] = {program, const_cast<char*>("game"), nullptr};
    mscharged::platform::StartSessionLog("test", 2, start);
    std::printf("game output\n");
    const int fd = open("/dev/null", O_RDONLY);
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, nullptr);
    std::string fd_text = std::to_string(fd);
    char* arguments[] = {program, const_cast<char*>("--reopened"), fd_text.data(), nullptr};
    mscharged::platform::ArmLauncherReopen(program, 3, arguments);
    mscharged::platform::ReopenLauncher("closed, reopening the launcher");
    std::_Exit(3); // execv failed
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--reopened") == 0) return Reopened(argc, argv);
    try
    {
        const auto program = std::filesystem::absolute(argv[0]);
        // Nothing armed, or the program is gone: the caller ends its log itself.
        Require(!mscharged::platform::ReopenLauncher("test"), "Reopened without being armed");
        char* missing[] = {argv[0], nullptr};
        mscharged::platform::ArmLauncherReopen("/nonexistent/recharged", 1, missing);
        Require(!mscharged::platform::ReopenLauncher("test"), "Reopened a missing program");

        const auto directory = std::filesystem::temp_directory_path()
            / ("recharged-reopen-test-" + std::to_string(getpid()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        std::string path = program.string();
        const pid_t pid = fork();
        Require(pid >= 0, "fork failed");
        if (pid == 0) Game(directory, path.data());
        int status = 0;
        Require(waitpid(pid, &status, 0) == pid, "waitpid failed");
        Require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "The reopened program did not pass its checks");
        const auto game = Read(directory / "recharged.previous.log");
        const auto launcher = Read(directory / "recharged.log");
        Require(game.find("game output") != std::string::npos
            && game.find("=== Session ended: closed, reopening the launcher ===") != std::string::npos,
            "The game's log lacks its output or closing line");
        Require(launcher.find("reopened output") != std::string::npos && launcher.find("--reopened") != std::string::npos
            && launcher.find("game output") == std::string::npos,
            "The reopened program's log lacks its output or arguments");
        std::filesystem::remove_all(directory);
        std::puts("Launcher reopen passed");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
