#pragma once

namespace mscharged::platform
{
// Windows: recharged.exe is a Windows (GUI) program, so Windows opens no
// console for it (#61). Started from cmd or PowerShell it writes to that
// console, whose prompt returns at once; otherwise stdout and stderr go to
// NUL, so the session log can still take them over. Redirected output
// (files, pipes) is kept. Other systems: nothing to do. Call first in main.
void ConnectStandardStreams();

// advanced.log_window (Windows and macOS): a separate window that follows
// this run's log. It is its own process (on macOS in Terminal), so closing
// it or selecting text in it cannot stop or stall the game; it ends with the
// game. Only a change of the setting opens or closes it: a window the player
// closed stays closed. Without a session log (development checks) or on
// Linux nothing opens.
void SetLogWindow(bool open);

// The log window's own process: `recharged --log-window LOG PID [STOP]`
// follows LOG until process PID ends (or the file STOP is removed).
bool IsLogWindowProcess(int argc, char** argv);
int RunLogWindow(int argc, char** argv);
} // namespace mscharged::platform
