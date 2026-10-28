#pragma once

// The network numbers of the performance overlay. The game module fills it
// (charged_netplay_snapshot); the host reads it after a game frame, on the
// thread that runs the game's tasks.
struct ChargedNetplaySnapshot
{
    // Connected to another machine (LAN or online, lobby or match): 1; 0
    // otherwise, logging in and searching too.
    int network;
    // The slowest connection's round trip in ms, as the game's own transport
    // measures it (a ping every 2 s, the other end's wait taken out); -1
    // without a connection.
    int round_trip_ms;
    // A network match with rollback: 1, and the two numbers below.
    int rollback;
    int input_delay_ms;  // this machine's input delay
    int predicted_steps; // the most steps predicted at once since the last read
};

#ifdef __cplusplus
extern "C" {
#endif
// Game module entry: fills `snapshot` when `size` matches this build's
// struct; false otherwise. Each call starts predicted_steps afresh.
typedef bool (*ChargedNetplaySnapshotFunction)(ChargedNetplaySnapshot* snapshot, unsigned size);
#ifdef __cplusplus
}
#endif
