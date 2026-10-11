#pragma once

#include <filesystem>
#include <string>

struct SDL_Window;

namespace mscharged::platform
{
// The log of this run: logs/mscharged.log beside the executable (or in the
// user's preference folder when that is not writable), with the previous
// run kept as mscharged.previous.log. Everything printed to stdout/stderr
// still reaches the console and is also written to the log, after a header
// with the version, command line, operating system, CPU and memory.
//
// It also records crashes: on Windows an unhandled exception, elsewhere a
// fatal signal, is logged with its cause and the call stack as module +
// offset, which the release's debug symbols turn back into source lines.
// Windows also writes mscharged-crash.dmp (a minidump) beside the log.
//
// MSCHARGED_LOG_DIR selects another folder; MSCHARGED_LOG_DIR=off disables
// the log. Call once, early in main, on the main thread.
void StartSessionLog(const char* version, int argc, char** argv);

// The log file, or an empty path without a log.
std::filesystem::path SessionLogPath();

// A line for the log only (settings and devices a player does not need on
// the console). A newline is added.
void SessionLogLine(const std::string& text);

// Displays, the video driver and the window, for the log only.
void LogSessionDisplays(SDL_Window* window);

// Writes everything printed so far to the log; call before std::_Exit.
void FlushSessionLog();

// An error that ends the program: printed, logged and, when the player has
// no console to read it (a Windows game started by double-click), shown in
// a message box that names the log.
void ReportFatalError(const std::string& message);
} // namespace mscharged::platform
