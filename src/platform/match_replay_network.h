#pragma once

// Match replays of network matches (match_replay.cpp), inside the original
// game module. In a network match the rollback (rollback_netplay.cpp) takes
// every step's inputs from the router, so the game's own recorder sees none:
// the rollback hands each step to the recording instead, once the step's
// inputs and those of every step before it have all arrived, in the order of
// the steps. A step comes as the recorder would have written it: the game's
// frame, checksum and random seed before the step ran (as it ran last), then
// each machine's remap angle, deterministic events and players' records.
//
// All on the game's thread, during the match: no allocation (the recording's
// memory is reserved when the match begins), no lock, no system call.

#include <cstdint>

namespace mscharged::replay
{
inline constexpr int kNetworkRecordSize = 16;  // PackedDetInput
inline constexpr int kNetworkEventSize = 0x24; // DetermDataEvent: its size byte, padding, 32 bytes

struct NetworkStepMachine
{
    int players = 0;
    std::uint16_t remap = 0;
    const std::uint8_t (*records)[kNetworkRecordSize] = nullptr;
    const std::uint8_t* states = nullptr;
    int events = 0;
    const std::uint8_t (*event_data)[kNetworkEventSize] = nullptr;
};

// Whether a network match is being recorded: the rollback hands its steps over.
bool RecordingNetworkMatch();
// Step `frame` of the match recorded: `before_frame`, `checksum` and `seed`
// as the router read them before the step's last run, and its machines'
// inputs in machine order.
void RecordNetworkStep(std::uint32_t before_frame, std::uint32_t checksum, std::uint32_t seed, int machine_count,
                       const NetworkStepMachine* machines);
} // namespace mscharged::replay
