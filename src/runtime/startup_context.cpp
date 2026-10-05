#include "runtime/startup.h"

namespace mscharged
{
[[noreturn]] void MissingStartupService(const char* symbol, const char* reason)
{ throw StartupStopped(symbol, reason); }
}
