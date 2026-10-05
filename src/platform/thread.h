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
}
