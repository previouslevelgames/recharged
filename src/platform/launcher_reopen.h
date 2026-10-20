#pragma once

#include <filesystem>

namespace mscharged::platform
{
// [launcher] reopen_after_game: when a game started from the launcher closes
// normally, the launcher opens again, with this run's arguments.
void ArmLauncherReopen(const std::filesystem::path& program, int argc, char** argv);

// Call where the game closes normally, in place of the session's last log
// line. Nothing armed (or the program is gone): returns false, and the caller
// ends its log as usual. Armed: ends the session log with `reason` and starts
// the launcher. Linux and macOS replace this process, which keeps its ID
// (Steam still sees its game), and return only when that fails; Windows starts
// a new process and returns, and the caller then ends this one.
bool ReopenLauncher(const char* reason);
}
