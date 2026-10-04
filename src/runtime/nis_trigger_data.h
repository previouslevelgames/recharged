#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mscharged
{
// Owned equivalent of the records appended by NisPlayer's trigger-definition
// interpreter. Type IDs and four words keep their original 32-bit meanings.
struct NisPlaybackTrigger
{
    std::uint32_t type = 0;
    float frame = 0;
    std::string name, target;
    float value = -1;
    std::array<std::uint32_t, 4> params{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
};
struct NisPlaybackTable
{
    unsigned render_mode = 0;
    std::vector<NisPlaybackTrigger> triggers;
};
}
