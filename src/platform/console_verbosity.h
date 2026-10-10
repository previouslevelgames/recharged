#pragma once

#include <atomic>

namespace mscharged::platform
{
// The console normally keeps what a player needs: the launch summary, the
// graphics adapter, controllers, warnings, errors and the results of keys
// they press. A verbose console (advanced.verbose_console) adds the port's
// development traces and the game's own debug output, which the Wii sends
// to its debug serial port. Tools and tests print everything; the game
// applies the setting at startup.
inline std::atomic<bool>& VerboseConsoleFlag()
{
    static std::atomic<bool> verbose{true};
    return verbose;
}
inline void SetVerboseConsole(bool verbose) { VerboseConsoleFlag().store(verbose, std::memory_order_relaxed); }
inline bool VerboseConsole() { return VerboseConsoleFlag().load(std::memory_order_relaxed); }
}
