#pragma once

#include <cstdint>

namespace mscharged
{
// Actual address limits for the calling host thread. Wii stackBegin is the
// upper address and stackEnd is the lower address; the game owns its test.
struct ThreadStackLimits
{
    std::uintptr_t low;
    std::uintptr_t high;
};

ThreadStackLimits CurrentThreadStackLimits();

// On Windows: keeps stack free for the crash report when the calling thread's
// stack overflows (session_log.cpp does the same for the main thread).
void ReserveStackForCrashReports();
}
