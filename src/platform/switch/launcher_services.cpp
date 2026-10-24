// The Switch program has no launcher and no log window (on PC these are
// launcher_reopen.cpp and log_window.cpp, which start desktop processes).
#include "platform/launcher_reopen.h"
#include "platform/log_window.h"

namespace mscharged::platform
{
// Nothing is ever armed: the game ends its session log itself.
bool ReopenLauncher(const char*)
{
    return false;
}

// The log stays in logs/recharged.log on the SD card.
void SetLogWindow(bool) {}
} // namespace mscharged::platform
