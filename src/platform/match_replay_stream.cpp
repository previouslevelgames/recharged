#include "platform/match_replay_stream.h"

#include <cstring>

namespace mscharged::replay
{
inline namespace MSCHARGED_REPLAY_CODEC
{
namespace
{
// A packet's flags, in the order its parts follow the flag byte.
constexpr std::uint8_t kFrame = 0x01;      // u32 frame: not the one after the machine's last
constexpr std::uint8_t kRemap = 0x02;      // u16 remap angle: changed
constexpr std::uint8_t kCheckpoint = 0x04; // u32 checksum, u32 seed
constexpr std::uint8_t kEvents = 0x08;     // u8 count; each: u8 size, its bytes
constexpr std::uint8_t kData = 0x10;       // u16 size; its bytes
constexpr std::uint8_t kRecords = 0x20;    // per player: 3-byte mask of the 17 bytes changed, those bytes
constexpr std::uint8_t kKnownFlags = 0x3F;
constexpr int kRecordBytes = kRecordSize + 1; // the record and its connection state

void Put8(Bytes& out, std::uint8_t value) { out.push_back(value); }
void Put16(Bytes& out, std::uint16_t value)
{
    out.push_back(std::uint8_t(value));
    out.push_back(std::uint8_t(value >> 8));
}
void Put32(Bytes& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) out.push_back(std::uint8_t(value >> shift));
}

void PutSetup(Bytes& out, const Setup& s)
{
    const auto i32 = [&](std::int32_t value) { Put32(out, std::uint32_t(value)); };
    i32(s.stadium);
    i32(s.home_team);
    for (const auto value : s.home_sidekicks) i32(value);
    i32(s.away_team);
    for (const auto value : s.away_sidekicks) i32(value);
    i32(s.skill);
    i32(s.win_by);
    i32(s.game_time);
    i32(s.game_goals);
    i32(s.best_series);
    for (const auto value : s.playing_sides) Put16(out, std::uint16_t(value));
    for (const auto value : s.settings) i32(value);
    for (const auto value : s.settings_flags) Put8(out, value);
    for (const auto value : s.cheats) i32(value);
    i32(s.mode);
    Put8(out, s.training);
    for (const auto value : s.difficulty) i32(value);
    Put32(out, s.tweaks);
    i32(s.training_item);
    i32(s.training_item_count);
    Put8(out, s.training_item_giant);
    i32(s.training_away_fielders);
    Put8(out, s.training_away_goalie);
    i32(s.training_away_skill);
    Put8(out, s.training_play_away);
    Put8(out, s.training_skip_goal);
    Put32(out, s.seed);
    i32(s.local_machine);
    i32(s.machine_count);
    for (const auto value : s.player_counts) i32(value);
    Put32(out, s.steps);
    for (const auto value : s.score) i32(value);
    // Network matches (later replays): read only when the setup has them.
    Put8(out, s.online);
    Put32(out, s.set_tweaks);
    Put32(out, s.sim_profile);
    Put32(out, std::uint32_t(std::uint64_t(s.series)));
    Put32(out, std::uint32_t(std::uint64_t(s.series) >> 32));
    i32(s.series_game);
}

// Reads from a bounded buffer; a read past its end fails and stays failed.
struct Cursor
{
    const std::uint8_t* data;
    std::size_t size;
    std::size_t at = 0;
    bool failed = false;

    bool Need(std::size_t count)
    {
        if (failed || size - at < count) failed = true;
        return !failed;
    }
    std::uint8_t Get8() { return Need(1) ? data[at++] : 0; }
    std::uint16_t Get16()
    {
        if (!Need(2)) return 0;
        const std::uint16_t value = std::uint16_t(data[at] | (data[at + 1] << 8));
        at += 2;
        return value;
    }
    std::uint32_t Get32()
    {
        if (!Need(4)) return 0;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= std::uint32_t(data[at + i]) << (8 * i);
        at += 4;
        return value;
    }
};

void GetSetup(Cursor& in, Setup& s)
{
    const auto i32 = [&] { return std::int32_t(in.Get32()); };
    s.stadium = i32();
    s.home_team = i32();
    for (auto& value : s.home_sidekicks) value = i32();
    s.away_team = i32();
    for (auto& value : s.away_sidekicks) value = i32();
    s.skill = i32();
    s.win_by = i32();
    s.game_time = i32();
    s.game_goals = i32();
    s.best_series = i32();
    for (auto& value : s.playing_sides) value = std::int16_t(in.Get16());
    for (auto& value : s.settings) value = i32();
    for (auto& value : s.settings_flags) value = in.Get8();
    for (auto& value : s.cheats) value = i32();
    s.mode = i32();
    s.training = in.Get8();
    for (auto& value : s.difficulty) value = i32();
    s.tweaks = in.Get32();
    s.training_item = i32();
    s.training_item_count = i32();
    s.training_item_giant = in.Get8();
    s.training_away_fielders = i32();
    s.training_away_goalie = in.Get8();
    s.training_away_skill = i32();
    s.training_play_away = in.Get8();
    s.training_skip_goal = in.Get8();
    s.seed = in.Get32();
    s.local_machine = i32();
    s.machine_count = i32();
    for (auto& value : s.player_counts) value = i32();
    s.steps = in.Get32();
    for (auto& value : s.score) value = i32();
    // A replay from before network matches stops here: an offline match.
    if (in.failed || in.at == in.size) return;
    s.online = in.Get8();
    s.set_tweaks = in.Get32();
    s.sim_profile = in.Get32();
    const std::uint64_t low = in.Get32();
    s.series = std::int64_t(low | std::uint64_t(in.Get32()) << 32);
    s.series_game = i32();
}

bool ValidCounts(const Setup& s)
{
    if (s.machine_count < 1 || s.machine_count > kMaxMachines) return false;
    if (s.local_machine < 0 || s.local_machine >= s.machine_count) return false;
    for (int machine = 0; machine < kMaxMachines; ++machine)
    {
        const int players = s.player_counts[machine];
        if (players < 0 || players > kMaxPlayers || (machine >= s.machine_count && players != 0)) return false;
    }
    return true;
}

// The stream's start: version, setup size, setup. On success `at` is the
// first step's byte.
bool ParseStart(Cursor& in, Setup& setup, std::string& error)
{
    const std::uint32_t version = in.Get32();
    const std::uint32_t setup_size = in.Get32();
    if (in.failed)
    {
        error = "The replay is too short.";
        return false;
    }
    if (version != kStreamVersion)
    {
        error = "The replay's format (" + std::to_string(version) + ") is not one this build reads.";
        return false;
    }
    if (!in.Need(setup_size))
    {
        error = "The replay is too short.";
        return false;
    }
    Cursor part{in.data + in.at, setup_size};
    GetSetup(part, setup);
    if (part.failed)
    {
        error = "The replay's setup is too short.";
        return false;
    }
    in.at += setup_size;
    if (!ValidCounts(setup))
    {
        error = "The replay's machines or players are out of range.";
        return false;
    }
    return true;
}
} // namespace

bool StreamWriter::Begin(const Setup& setup)
{
    Clear();
    if (!ValidCounts(setup)) return false;
    setup_ = setup;
    setup_.steps = 0;
    active_ = true;
    return true;
}

void StreamWriter::Clear()
{
    active_ = broken_ = pending_ = false;
    setup_ = Setup{};
    steps_.clear();
    body_.clear();
    records_bytes_.clear();
    std::memset(expected_frame_, 0, sizeof expected_frame_);
    std::memset(frame_known_, 0, sizeof frame_known_);
    std::memset(remap_, 0, sizeof remap_);
    std::memset(previous_, 0, sizeof previous_);
}

void StreamWriter::Header(int machine, std::uint16_t remap, std::uint32_t checksum, std::uint32_t frame,
                          std::uint32_t seed, unsigned events, unsigned data_size)
{
    if (!active_) return;
    Flush();
    if (!active_) return;
    if (machine < 0 || machine >= setup_.machine_count || events > unsigned(kMaxEvents)
        || data_size > unsigned(kMaxDataSize))
    {
        broken_ = true;
        active_ = false;
        return;
    }
    pending_ = true;
    machine_ = machine;
    records_ = 0;
    events_expected_ = events;
    events_written_ = 0;
    data_expected_ = data_size;
    data_written_ = false;
    changed_ = false;
    flags_ = 0;
    body_.clear();
    records_bytes_.clear();
    if (!frame_known_[machine] || frame != expected_frame_[machine])
    {
        flags_ |= kFrame;
        Put32(body_, frame);
    }
    frame_known_[machine] = true;
    expected_frame_[machine] = frame + 1;
    if (remap != remap_[machine])
    {
        flags_ |= kRemap;
        Put16(body_, remap);
        remap_[machine] = remap;
    }
    if (frame % kCheckpointSteps == 0)
    {
        flags_ |= kCheckpoint;
        Put32(body_, checksum);
        Put32(body_, seed);
    }
    if (events)
    {
        flags_ |= kEvents;
        Put8(body_, std::uint8_t(events));
    }
    if (data_size)
    {
        flags_ |= kData;
        Put16(body_, std::uint16_t(data_size));
    }
}

void StreamWriter::Event(const std::uint8_t* data, unsigned size)
{
    if (!active_) return;
    if (!pending_ || events_written_ >= events_expected_ || size > unsigned(kMaxEventSize) || (size && !data))
    {
        broken_ = true;
        active_ = false;
        return;
    }
    Put8(body_, std::uint8_t(size));
    body_.insert(body_.end(), data, data + size);
    ++events_written_;
}

void StreamWriter::Data(const std::uint8_t* data, unsigned size)
{
    if (!active_ || size == 0) return;
    if (!pending_ || data_written_ || events_written_ != events_expected_ || size != data_expected_ || !data)
    {
        broken_ = true;
        active_ = false;
        return;
    }
    body_.insert(body_.end(), data, data + size);
    data_written_ = true;
}

void StreamWriter::Record(int machine, int player, const std::uint8_t* record, std::uint8_t connected)
{
    if (!active_) return;
    if (!pending_ || machine != machine_ || player != records_ || player >= setup_.player_counts[machine] || !record
        || events_written_ != events_expected_ || data_written_ != (data_expected_ != 0))
    {
        broken_ = true;
        active_ = false;
        return;
    }
    std::uint8_t* previous = previous_[machine * kMaxPlayers + player];
    std::uint32_t mask = 0;
    std::uint8_t changes[kRecordBytes];
    int count = 0;
    for (int i = 0; i < kRecordBytes; ++i)
    {
        const std::uint8_t value = i < kRecordSize ? record[i] : connected;
        if (value != previous[i])
        {
            mask |= 1u << i;
            changes[count++] = value;
            previous[i] = value;
        }
    }
    Put8(records_bytes_, std::uint8_t(mask));
    Put8(records_bytes_, std::uint8_t(mask >> 8));
    Put8(records_bytes_, std::uint8_t(mask >> 16));
    records_bytes_.insert(records_bytes_.end(), changes, changes + count);
    changed_ = changed_ || mask != 0;
    ++records_;
}

void StreamWriter::Flush()
{
    if (!pending_) return;
    pending_ = false;
    if (records_ != setup_.player_counts[machine_] || events_written_ != events_expected_
        || data_written_ != (data_expected_ != 0))
    {
        // A packet the game left unfinished: the stream keeps what came before.
        broken_ = true;
        active_ = false;
        return;
    }
    if (changed_) flags_ |= kRecords;
    Put8(steps_, flags_);
    steps_.insert(steps_.end(), body_.begin(), body_.end());
    if (changed_) steps_.insert(steps_.end(), records_bytes_.begin(), records_bytes_.end());
    if (machine_ == 0) ++setup_.steps;
}

void StreamWriter::StartBytes(Bytes& out, bool live) const
{
    Bytes setup;
    Setup start = setup_;
    if (live) start.steps = 0;
    PutSetup(setup, start);
    Put32(out, kStreamVersion);
    Put32(out, std::uint32_t(setup.size()));
    out.insert(out.end(), setup.begin(), setup.end());
}

Bytes StreamWriter::Finish()
{
    Flush();
    Bytes setup;
    PutSetup(setup, setup_);
    Bytes out;
    out.reserve(8 + setup.size() + steps_.size());
    Put32(out, kStreamVersion);
    Put32(out, std::uint32_t(setup.size()));
    out.insert(out.end(), setup.begin(), setup.end());
    out.insert(out.end(), steps_.begin(), steps_.end());
    Clear();
    return out;
}

bool ReadSetup(const std::uint8_t* data, std::size_t size, Setup& setup, std::string& error)
{
    Cursor in{data, data ? size : 0};
    return ParseStart(in, setup, error);
}

bool StreamReader::Open(const std::uint8_t* data, std::size_t size, std::string& error)
{
    *this = StreamReader{};
    Cursor in{data, data ? size : 0};
    if (!ParseStart(in, setup_, error))
    {
        error_ = error;
        return false;
    }
    data_ = data;
    size_ = size;
    at_ = in.at;
    return true;
}

int StreamReader::StepsAhead(int most) const
{
    if (!data_ || !error_.empty() || events_left_ || data_left_ || records_left_) return 0;
    StreamReader probe = *this;
    std::uint8_t scratch[kMaxDataSize > kRecordSize ? kMaxDataSize : kRecordSize];
    int steps = 0;
    for (; steps < most; ++steps)
        for (int machine = 0; machine < setup_.machine_count; ++machine)
        {
            PacketHeader header;
            if (!probe.Header(machine, header)) return steps;
            for (unsigned event = 0; event < header.events; ++event)
            {
                unsigned size = 0;
                if (!probe.Event(scratch, size)) return steps;
            }
            if (!probe.Data(scratch, header.data_size)) return steps;
            std::uint8_t connected = 0;
            for (int player = 0; player < setup_.player_counts[machine]; ++player)
                if (!probe.Record(machine, player, scratch, connected)) return steps;
        }
    return steps;
}

bool StreamReader::Need(std::size_t count)
{
    if (!error_.empty()) return false;
    if (size_ - at_ < count)
    {
        error_ = "The replay ends in the middle of a step.";
        return false;
    }
    return true;
}

bool StreamReader::Header(int machine, PacketHeader& header)
{
    if (!error_.empty() || !data_) return false;
    if (events_left_ || data_left_ || records_left_)
    {
        error_ = "The game read a replay step out of order.";
        return false;
    }
    if (at_ == size_) return false; // the end
    if (machine < 0 || machine >= setup_.machine_count)
    {
        error_ = "The game asked the replay for a machine it does not have.";
        return false;
    }
    Cursor in{data_, size_, at_};
    const std::uint8_t flags = in.Get8();
    if (flags & ~kKnownFlags)
    {
        error_ = "The replay has a step this build cannot read.";
        return false;
    }
    header = PacketHeader{};
    if (flags & kFrame) header.frame = in.Get32();
    else if (!frame_known_[machine])
    {
        error_ = "The replay's first step has no frame.";
        return false;
    }
    else header.frame = expected_frame_[machine];
    if (flags & kRemap) remap_[machine] = in.Get16();
    header.remap = remap_[machine];
    if (flags & kCheckpoint)
    {
        header.checkpoint = true;
        header.checksum = in.Get32();
        header.seed = in.Get32();
    }
    if (flags & kEvents)
    {
        header.events = in.Get8();
        if (!header.events)
        {
            error_ = "The replay has a step with an empty event list.";
            return false;
        }
    }
    if (flags & kData)
    {
        header.data_size = in.Get16();
        if (!header.data_size || header.data_size > unsigned(kMaxDataSize))
        {
            error_ = "The replay has a step with network data out of range.";
            return false;
        }
    }
    if (in.failed)
    {
        error_ = "The replay ends in the middle of a step.";
        return false;
    }
    at_ = in.at;
    frame_known_[machine] = true;
    expected_frame_[machine] = header.frame + 1;
    events_left_ = header.events;
    data_left_ = header.data_size;
    records_left_ = unsigned(setup_.player_counts[machine]);
    records_changed_ = (flags & kRecords) != 0;
    machine_ = machine;
    next_player_ = 0;
    if (machine == 0) ++step_;
    return true;
}

bool StreamReader::Event(std::uint8_t* data, unsigned& size)
{
    if (!error_.empty()) return false;
    if (!events_left_)
    {
        error_ = "The game read more events than the replay step has.";
        return false;
    }
    if (!Need(1)) return false;
    size = data_[at_];
    if (size > unsigned(kMaxEventSize))
    {
        error_ = "The replay has an event larger than the game's.";
        return false;
    }
    if (!Need(1 + std::size_t(size))) return false;
    if (size) std::memcpy(data, data_ + at_ + 1, size);
    at_ += 1 + size;
    --events_left_;
    return true;
}

bool StreamReader::Data(std::uint8_t* data, unsigned size)
{
    if (!error_.empty()) return false;
    if (size == 0 && data_left_ == 0) return true;
    if (events_left_ || size != data_left_)
    {
        error_ = "The game read network data the replay step does not have.";
        return false;
    }
    if (!Need(size)) return false;
    std::memcpy(data, data_ + at_, size);
    at_ += size;
    data_left_ = 0;
    return true;
}

bool StreamReader::Record(int machine, int player, std::uint8_t* record, std::uint8_t& connected)
{
    if (!error_.empty()) return false;
    if (machine != machine_ || player != next_player_ || !records_left_ || events_left_ || data_left_)
    {
        error_ = "The game read a replay player out of order.";
        return false;
    }
    std::uint8_t* previous = previous_[machine * kMaxPlayers + player];
    if (records_changed_)
    {
        if (!Need(3)) return false;
        const std::uint32_t mask = std::uint32_t(data_[at_]) | (std::uint32_t(data_[at_ + 1]) << 8)
            | (std::uint32_t(data_[at_ + 2]) << 16);
        if (mask >> kRecordBytes)
        {
            error_ = "The replay has a player record this build cannot read.";
            return false;
        }
        at_ += 3;
        for (int i = 0; i < kRecordBytes; ++i)
            if (mask & (1u << i))
            {
                if (!Need(1)) return false;
                previous[i] = data_[at_++];
            }
    }
    std::memcpy(record, previous, kRecordSize);
    connected = previous[kRecordSize];
    --records_left_;
    ++next_player_;
    return true;
}
} // inline namespace MSCHARGED_REPLAY_CODEC
} // namespace mscharged::replay
