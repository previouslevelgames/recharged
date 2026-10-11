// logs/mscharged.log: console output is copied to the log, and a crash is
// recorded with its signal and call stack before the process ends as it would.
#include "platform/session_log.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string Read(const std::string& path)
{
    std::ifstream input(path);
    std::stringstream text;
    text << input.rdbuf();
    return text.str();
}

// Runs `mode` in a child with its own log folder; returns its wait status.
int Child(const std::string& directory, const char* mode, char* program)
{
    const pid_t pid = fork();
    Require(pid >= 0, "fork failed");
    if (pid == 0)
    {
        setenv("MSCHARGED_LOG_DIR", directory.c_str(), 1);
        char* argv[] = {program, const_cast<char*>(mode), nullptr};
        mscharged::platform::StartSessionLog("test", 2, argv);
        std::printf("stdout before %s\n", mode);
        std::fprintf(stderr, "stderr before %s\n", mode);
        mscharged::platform::SessionLogLine("log-only line");
        if (std::string(mode) == "crash") *(volatile int*)nullptr = 1;
        if (std::string(mode) == "fatal")
        {
            mscharged::platform::ReportFatalError("fatal test message");
            std::_Exit(1);
        }
        std::exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}
} // namespace

int main(int, char** argv)
{
    try
    {
        char directory_template[] = "/tmp/mscharged-session-log-XXXXXX";
        Require(mkdtemp(directory_template), "mkdtemp failed");
        const std::string directory = directory_template;
        const std::string log = directory + "/mscharged.log";

        int status = Child(directory, "normal", argv[0]);
        Require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Normal run did not exit 0");
        auto text = Read(log);
        Require(text.find("=== Mario Strikers Charged native port test ===") != std::string::npos, "No log header");
        Require(text.find("stdout before normal") != std::string::npos
                    && text.find("stderr before normal") != std::string::npos,
                "Console output missing from the log");
        Require(text.find("log-only line") != std::string::npos, "Log-only line missing");
        Require(text.find("=== Session ended ===") != std::string::npos, "Normal exit not logged");

        status = Child(directory, "fatal", argv[0]);
        Require(WIFEXITED(status) && WEXITSTATUS(status) == 1, "Fatal run did not exit 1");
        Require(Read(log).find("fatal test message") != std::string::npos, "Fatal error missing before _Exit");
        Require(Read(directory + "/mscharged.previous.log").find("stdout before normal") != std::string::npos,
                "Previous log not kept");

        status = Child(directory, "crash", argv[0]);
        Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "Crash did not end with SIGSEGV");
        text = Read(log);
        Require(text.find("stdout before crash") != std::string::npos, "Output before the crash missing");
        Require(text.find("*** Crash: signal 11") != std::string::npos, "Crash not logged");
#if __has_include(<execinfo.h>)
        Require(text.find("Call stack:") != std::string::npos && text.find("#01 ") != std::string::npos,
                "Crash stack missing");
#endif
        std::printf("Session log: console copy, log-only lines, normal/fatal exits, previous log and crash report pass.\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Session log: %s\n", error.what());
        return 1;
    }
}
