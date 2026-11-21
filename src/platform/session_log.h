#pragma once

#include <filesystem>
#include <string>

struct SDL_Window;

namespace mscharged::platform
{
// The log of this run: logs/recharged.log beside the executable (or in the
// user's preference folder when that is not writable), with the previous
// run kept as recharged.previous.log. Everything printed to stdout/stderr
// still reaches the console and is also written to the log, after a header
// with the version, command line, operating system, CPU and memory.
//
// It also records crashes: on Windows an unhandled exception, elsewhere a
// fatal signal, is logged with its cause and the call stack as module +
// offset, which the release's debug symbols turn back into source lines.
// Windows also writes recharged-crash.dmp (a minidump) beside the log, also
// after a stack overflow (the stack keeps room for the report).
//
// RECHARGED_LOG_DIR selects another folder; RECHARGED_LOG_DIR=off disables
// the log. Call once, early in main, on the main thread.
void StartSessionLog(const char* version, int argc, char** argv);

// The log file, or an empty path without a log.
std::filesystem::path SessionLogPath();

// A line for the log only (settings and devices a player does not need on
// the console). A newline is added.
void SessionLogLine(const std::string& text);
// [advanced] log_level = debug: every line of the log from now on begins with
// its local time to the millisecond ("09:12:03.412 "), the game's printed
// output included. The console stays as it is.
void SetSessionLogTimes(bool on);
bool SessionLogTimes();
// The local time as those lines begin with it.
std::string LogTimeStamp();

// Borrowed, null-terminated text from a game module. Any string allocation and
// destruction stays in the host, which may use a different C++ allocator.
void SessionLogText(const char* text);

// Displays, the video driver and the window, for the log only.
void LogSessionDisplays(SDL_Window* window);

// The freeze watch: call after each frame the game finishes, on the thread
// that runs the frames. The first call starts a watcher that notes in the log
// when no frame has finished for 5 s (and again at 20 s), on Windows with that
// thread's call stack, and when frames come again. A game that freezes and is
// then closed still leaves where it stood.
void FrameFinished();

// No frames are expected any more (shutdown, an error box): the watch ends.
void StopFreezeWatch();

// Writes everything printed so far to the log; call before TerminateProcessNow.
void FlushSessionLog();

// Before the program starts itself again (the launcher after an update):
// writes what is pending and a closing line with `reason`, then gives stdout
// and stderr back to the console, so the next program does not inherit this
// run's pipes. Nothing is logged afterwards.
void EndSessionLog(const char* reason);

// An error that ends the program: printed, logged and, when the player has
// no console to read it (a Windows game started by double-click), shown in
// a message box that names the log.
void ReportFatalError(const std::string& message);
} // namespace mscharged::platform
