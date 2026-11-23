#pragma once

// Match replays: a match as the original game's own input recorder gives it
// (NetworkInputRecording: the match's setup and random seed, then for every
// fixed game step the inputs the router applies), in a compact stream.
//
// The game module writes the stream while a match is recorded and reads it
// again when a replay plays (src/platform/match_replay.cpp, tweak patch 0071);
// the host keeps it in a .msc file (match_replay_file.h). Nothing here touches
// the game: the codec is plain bytes, tested by tests/match_replay.cpp.
//
// A step is one packet per machine (an offline match has one): its header
// (the remap angle, the checksum and seed, the frame), the deterministic
// events, the network data (online only) and one 16-byte PackedDetInput
// record with its connection state per player. CPU players have no inputs:
// the game decides their play from the same state and seed. The stream keeps
// what changes: a packet is one flag byte when nothing changed, and the
// checksum and seed are kept every kCheckpointSteps steps.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <vector>

// The game module compiles a copy of this codec. On Windows it links against
// recharged.exe, which exports all its symbols: the module's copy takes names
// of its own, or the exe's would clash with it.
#if defined(MSCHARGED_GAME_MODULE)
#define MSCHARGED_REPLAY_CODEC game_module
#else
#define MSCHARGED_REPLAY_CODEC program
#endif

namespace mscharged::replay
{
inline namespace MSCHARGED_REPLAY_CODEC
{
// Bytes from malloc: in the game module, new and the standard containers'
// default allocator take the game's own heap.
template <class T>
struct MallocAllocator
{
    using value_type = T;
    MallocAllocator() = default;
    template <class U>
    MallocAllocator(const MallocAllocator<U>&) noexcept
    {
    }
    T* allocate(std::size_t count)
    {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        if (void* memory = std::malloc(count * sizeof(T))) return static_cast<T*>(memory);
        throw std::bad_alloc();
    }
    void deallocate(T* memory, std::size_t) noexcept { std::free(memory); }
    template <class U>
    bool operator==(const MallocAllocator<U>&) const noexcept
    {
        return true;
    }
    template <class U>
    bool operator!=(const MallocAllocator<U>&) const noexcept
    {
        return false;
    }
};
using Bytes = std::vector<std::uint8_t, MallocAllocator<std::uint8_t>>;

inline constexpr std::uint32_t kStreamVersion = 1;
inline constexpr int kMaxMachines = 4;
inline constexpr int kMaxPlayers = 4;      // per machine
inline constexpr int kRecordSize = 16;     // PackedDetInput
inline constexpr int kMaxEventSize = 32;   // DetermDataEvent's data
inline constexpr int kMaxEvents = 255;     // per packet
inline constexpr int kMaxDataSize = 300;   // the router's serialized network message
inline constexpr std::uint32_t kCheckpointSteps = 25;

// What the match starts from: the game's own recorded configuration
// (RecordedGameConfig) and what the port adds so that the match is the same
// again: the whole gameplay settings and cheats, the mode, the tweaks and
// Training's settings, and the difficulty the loading chose. Integers as the
// game keeps them; the stream stores each in little-endian order.
struct Setup
{
    // RecordedGameConfig.
    std::int32_t stadium = 0;
    std::int32_t home_team = 0;
    std::int32_t home_sidekicks[3] = {};
    std::int32_t away_team = 0;
    std::int32_t away_sidekicks[3] = {};
    std::int32_t skill = 0;
    std::int32_t win_by = 0;
    std::int32_t game_time = 0;
    std::int32_t game_goals = 0;
    std::int32_t best_series = 0;
    std::int16_t playing_sides[16] = {};
    // The match's whole GameplaySettings (mCurGameGameplayOptions): the five
    // above and the power-up, Mega Strike and Skillshot switches.
    std::int32_t settings[5] = {};
    std::uint8_t settings_flags[7] = {};
    // The active CheatSettings: custom power-ups, environment, player.
    std::int32_t cheats[3] = {};
    // GameInfoManager's mode (eGameMode) and whether Training's match.
    std::int32_t mode = 0;
    std::uint8_t training = 0;
    // The difficulty of each side once the loading chose it.
    std::int32_t difficulty[2] = {};
    // The port's tweaks (bits of mscharged::GameTweak) the match played with.
    std::uint32_t tweaks = 0;
    // Training: the item (ePowerUpType, -1 none), how many and whether giant,
    // the opponent's fielders, goalie and skill, the side that trains, and
    // whether a goal goes straight to the kickoff.
    std::int32_t training_item = -1;
    std::int32_t training_item_count = 1;
    std::uint8_t training_item_giant = 0;
    std::int32_t training_away_fielders = 0;
    std::uint8_t training_away_goalie = 1;
    std::int32_t training_away_skill = -1;
    std::uint8_t training_play_away = 0;
    std::uint8_t training_skip_goal = 0;
    // The recorder's header: seed, machines and their players.
    std::uint32_t seed = 0;
    std::int32_t local_machine = 0;
    std::int32_t machine_count = 1;
    std::int32_t player_counts[kMaxMachines] = {};
    // How it went: the steps recorded, and the score when it ended.
    std::uint32_t steps = 0;
    std::int32_t score[2] = {};
    // A network match (online or LAN): the steps are every machine's inputs
    // as the rollback confirmed them, and a replay plays by the rules the
    // match had online (the tweaks it switches off or on there, the set's).
    // Replays before these fields read as an offline match.
    std::uint8_t online = 0;
    // The set's tweaks (bits of mscharged::GameTweak) a set's match plays with,
    // whatever each player chose; 0 outside a set.
    std::uint32_t set_tweaks = 0;
    // The simulation profile the match ran by (mscharged_sim_profile.h), 0
    // for the module's own.
    std::uint32_t sim_profile = 0;
    // Its series as the match reports name it (its start, Unix seconds by the
    // game's clock) and the game of the series, from 1; 0 outside one.
    std::int64_t series = 0;
    std::int32_t series_game = 0;
};

// One machine's packet of a step, as the recorder reads it back.
struct PacketHeader
{
    std::uint16_t remap = 0;
    std::uint32_t frame = 0;
    bool checkpoint = false; // checksum and seed below are the recorded ones
    std::uint32_t checksum = 0;
    std::uint32_t seed = 0;
    unsigned events = 0;
    unsigned data_size = 0;
};

// Writes a match in the order the game's recorder hands it over: Header,
// its events and data, then each player's Record; Finish gives the stream.
class StreamWriter
{
public:
    // False (nothing written) for counts out of range.
    bool Begin(const Setup& setup);
    // Reserves `bytes` for the steps, so that writing them allocates nothing
    // until they pass it.
    void Reserve(std::size_t bytes) { steps_.reserve(bytes); }
    bool Active() const { return active_; }
    void Header(int machine, std::uint16_t remap, std::uint32_t checksum, std::uint32_t frame, std::uint32_t seed,
                unsigned events, unsigned data_size);
    void Event(const std::uint8_t* data, unsigned size);
    void Data(const std::uint8_t* data, unsigned size);
    void Record(int machine, int player, const std::uint8_t* record, std::uint8_t connected);
    Setup& EditSetup() { return setup_; }
    // The last packet into the steps once its last player's record came (a
    // packet otherwise waits for the next one's Header): after a whole step,
    // StepBytes holds every step so far.
    void Settle() { Flush(); }
    // The stream's start as it would be now (version, setup size, setup), and
    // the steps written so far, which only grow until Finish or Clear: what a
    // live stream sends while the match is played.
    void StartBytes(Bytes& out) const;
    const Bytes& StepBytes() const { return steps_; }
    // The stream (setup, then the steps); the writer is empty again.
    Bytes Finish();
    void Clear();
    std::uint32_t Steps() const { return setup_.steps; }
    // A packet's parts came out of order or over their limits; Finish keeps
    // the steps before it.
    bool Broken() const { return broken_; }

private:
    void Flush();

    bool active_ = false;
    bool broken_ = false;
    Setup setup_;
    Bytes steps_;
    // The packet being collected, until its last player's record.
    bool pending_ = false;
    int machine_ = 0;
    int records_ = 0;
    unsigned events_expected_ = 0;
    unsigned events_written_ = 0;
    unsigned data_expected_ = 0;
    bool data_written_ = false;
    std::uint8_t flags_ = 0;
    Bytes body_;
    Bytes records_bytes_;
    bool changed_ = false;
    std::uint32_t expected_frame_[kMaxMachines] = {};
    bool frame_known_[kMaxMachines] = {};
    std::uint16_t remap_[kMaxMachines] = {};
    std::uint8_t previous_[kMaxMachines * kMaxPlayers][kRecordSize + 1] = {};
};

// Reads a stream back in the same order.
class StreamReader
{
public:
    // False with `error` set for a stream this build cannot read.
    bool Open(const std::uint8_t* data, std::size_t size, std::string& error);
    const Setup& GetSetup() const { return setup_; }
    // The next packet of `machine`; false at the end of the stream (or when
    // the stream is broken: Error says).
    bool Header(int machine, PacketHeader& header);
    bool Event(std::uint8_t* data, unsigned& size);
    bool Data(std::uint8_t* data, unsigned size);
    bool Record(int machine, int player, std::uint8_t* record, std::uint8_t& connected);
    // Steps read so far (headers of machine 0).
    std::uint32_t Step() const { return step_; }
    const std::string& Error() const { return error_; }
    // A live stream: the bytes from Open's start on that have arrived now
    // (more of the same buffer; never fewer).
    void Grow(std::size_t size)
    {
        if (size > size_) size_ = size;
    }
    // The whole steps ahead of the reader in the bytes it has, at most
    // `most`: read on a copy, so the reader itself moves on only when the game
    // reads, and never into a step that has not arrived whole. Only between
    // steps.
    int StepsAhead(int most) const;

private:
    bool Need(std::size_t count);

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t at_ = 0;
    Setup setup_;
    std::string error_;
    std::uint32_t step_ = 0;
    // The packet being read: its events and data left, whether its records changed.
    unsigned events_left_ = 0;
    unsigned data_left_ = 0;
    unsigned records_left_ = 0;
    bool records_changed_ = false;
    int machine_ = 0;
    int next_player_ = 0;
    std::uint32_t expected_frame_[kMaxMachines] = {};
    bool frame_known_[kMaxMachines] = {};
    std::uint16_t remap_[kMaxMachines] = {};
    std::uint8_t previous_[kMaxMachines * kMaxPlayers][kRecordSize + 1] = {};
};

// The setup alone, as the stream begins with it (for the host's checks and
// listings); false with `error` set otherwise.
bool ReadSetup(const std::uint8_t* data, std::size_t size, Setup& setup, std::string& error);
} // inline namespace MSCHARGED_REPLAY_CODEC
} // namespace mscharged::replay
