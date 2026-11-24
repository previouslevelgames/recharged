// Match replays: the compact stream round trip (what the game's recorder hands
// over comes back the same, in the same order), damaged streams and files are
// refused without reading past their end, and the .msc file's packing, names
// and pruning.
#include "platform/live_ring.h"
#include "platform/match_replay_file.h"
#include "platform/match_replay_stream.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mscharged::replay;

namespace
{
int sChecks = 0;
void Require(bool condition, const std::string& message)
{
    ++sChecks;
    if (!condition) throw std::runtime_error(message);
}

// One machine's packet of one step, as the recorder sees it.
struct Packet
{
    int machine = 0;
    std::uint16_t remap = 0;
    std::uint32_t checksum = 0, frame = 0, seed = 0;
    std::vector<std::vector<std::uint8_t>> events;
    std::vector<std::uint8_t> data;
    std::vector<std::array<std::uint8_t, 16>> records;
    std::vector<std::uint8_t> connected;
};

Setup MakeSetup(int machines, int players)
{
    Setup setup;
    setup.stadium = 13;
    setup.home_team = 2;
    setup.away_team = 7;
    setup.home_sidekicks[0] = 1;
    setup.away_sidekicks[2] = 3;
    setup.playing_sides[0] = 0;
    setup.playing_sides[1] = 1;
    for (int pad = 2; pad < 16; ++pad) setup.playing_sides[pad] = -1;
    setup.settings[0] = 4;
    setup.settings_flags[3] = 1;
    setup.cheats[1] = 2;
    setup.mode = 0;
    setup.training = 1;
    setup.difficulty[0] = 7;
    setup.difficulty[1] = 4;
    setup.tweaks = 0x1234;
    setup.training_item = 9;
    setup.seed = 0xC0FFEE11;
    setup.machine_count = machines;
    for (int machine = 0; machine < machines; ++machine) setup.player_counts[machine] = players;
    setup.score[0] = 3;
    setup.score[1] = 2;
    return setup;
}

// A match whose inputs mostly stay the same, as real ones do: the sticks
// move now and then, buttons go down and up, an event every few seconds.
std::vector<Packet> MakeMatch(const Setup& setup, int steps, std::uint32_t first_frame, unsigned seed)
{
    std::mt19937 random(seed);
    std::vector<Packet> packets;
    std::array<std::array<std::uint8_t, 16>, 16> current{};
    std::array<std::uint16_t, 4> remap{};
    for (int step = 0; step < steps; ++step)
        for (int machine = 0; machine < setup.machine_count; ++machine)
        {
            Packet packet;
            packet.machine = machine;
            packet.frame = first_frame + std::uint32_t(step) + (step > steps / 2 ? 7 : 0); // one jump
            packet.checksum = random();
            packet.seed = random();
            if (random() % 200 == 0) remap[machine] = std::uint16_t(random());
            packet.remap = remap[machine];
            if (random() % 100 == 0)
                for (unsigned n = 1 + random() % 3; n > 0; --n)
                {
                    std::vector<std::uint8_t> event(random() % 33);
                    for (auto& byte : event) byte = std::uint8_t(random());
                    packet.events.push_back(event);
                }
            if (setup.machine_count > 1 && random() % 50 == 0)
            {
                packet.data.resize(1 + random() % 300);
                for (auto& byte : packet.data) byte = std::uint8_t(random());
            }
            for (int player = 0; player < setup.player_counts[machine]; ++player)
            {
                auto& record = current[std::size_t(machine * 4 + player)];
                if (random() % 10 == 0) record[std::size_t(random() % 16)] = std::uint8_t(random());
                packet.records.push_back(record);
                packet.connected.push_back(std::uint8_t(random() % 300 == 0 ? 2 : 1));
            }
            packets.push_back(packet);
        }
    return packets;
}

Bytes Record(const Setup& setup, const std::vector<Packet>& packets)
{
    StreamWriter writer;
    Require(writer.Begin(setup), "the writer takes the setup");
    for (const auto& packet : packets)
    {
        writer.Header(packet.machine, packet.remap, packet.checksum, packet.frame, packet.seed,
                      unsigned(packet.events.size()), unsigned(packet.data.size()));
        for (const auto& event : packet.events) writer.Event(event.data(), unsigned(event.size()));
        writer.Data(packet.data.data(), unsigned(packet.data.size()));
        for (std::size_t player = 0; player < packet.records.size(); ++player)
            writer.Record(packet.machine, int(player), packet.records[player].data(), packet.connected[player]);
    }
    Require(!writer.Broken(), "the writer took every packet");
    return writer.Finish();
}

// Plays the stream back the way the game reads it and compares.
void Replay(const Bytes& stream, const Setup& setup, const std::vector<Packet>& packets)
{
    StreamReader reader;
    std::string error;
    Require(reader.Open(stream.data(), stream.size(), error), "the reader opens the stream: " + error);
    const Setup& read = reader.GetSetup();
    Require(read.stadium == setup.stadium && read.home_team == setup.home_team && read.away_team == setup.away_team,
            "the teams and stadium come back");
    Require(std::memcmp(read.playing_sides, setup.playing_sides, sizeof setup.playing_sides) == 0,
            "the playing sides come back");
    Require(read.seed == setup.seed && read.tweaks == setup.tweaks && read.training == setup.training
                && read.training_item == setup.training_item && read.difficulty[1] == setup.difficulty[1]
                && read.cheats[1] == setup.cheats[1] && read.settings_flags[3] == 1 && read.score[0] == 3,
            "the setup's other fields come back");
    Require(read.steps * std::uint32_t(setup.machine_count) == packets.size(), "the step count is the packets'");
    for (const auto& packet : packets)
    {
        PacketHeader header;
        Require(reader.Header(packet.machine, header), "a packet's header: " + reader.Error());
        Require(header.frame == packet.frame, "the frame comes back");
        Require(header.remap == packet.remap, "the remap angle comes back");
        Require(header.checkpoint == (packet.frame % kCheckpointSteps == 0), "checkpoints every 25 frames");
        if (header.checkpoint)
            Require(header.checksum == packet.checksum && header.seed == packet.seed, "a checkpoint's values");
        Require(header.events == packet.events.size() && header.data_size == packet.data.size(),
                "the event and data counts come back");
        for (const auto& event : packet.events)
        {
            std::uint8_t data[kMaxEventSize];
            unsigned size = 0;
            Require(reader.Event(data, size), "an event");
            Require(size == event.size() && (!size || std::memcmp(data, event.data(), size) == 0), "the event's bytes");
        }
        std::uint8_t data[kMaxDataSize];
        Require(reader.Data(data, unsigned(packet.data.size())), "the network data");
        Require(packet.data.empty() || std::memcmp(data, packet.data.data(), packet.data.size()) == 0,
                "the network data's bytes");
        for (std::size_t player = 0; player < packet.records.size(); ++player)
        {
            std::uint8_t record[16];
            std::uint8_t connected = 0;
            Require(reader.Record(packet.machine, int(player), record, connected), "a record");
            Require(std::memcmp(record, packet.records[player].data(), 16) == 0
                        && connected == packet.connected[player],
                    "the record and its state come back");
        }
    }
    PacketHeader end;
    Require(!reader.Header(0, end) && reader.Error().empty(), "the stream ends cleanly");
}

void RoundTrips()
{
    const Setup offline = MakeSetup(1, 4);
    const auto match = MakeMatch(offline, 9000, 0, 1);
    const Bytes stream = Record(offline, match);
    Replay(stream, offline, match);
    // A three-minute match of four players: a few bytes a step before zlib.
    std::printf("offline 9000 steps: %zu bytes\n", stream.size());
    Require(stream.size() < 9000 * 12, "the stream stays compact");

    const Setup online = MakeSetup(2, 2);
    const auto networked = MakeMatch(online, 3000, 120, 2);
    Replay(Record(online, networked), online, networked);

    // No players at all (a machine of CPU teams), and a frame that starts late.
    Setup empty = MakeSetup(1, 0);
    const auto quiet = MakeMatch(empty, 100, 33, 3);
    Replay(Record(empty, quiet), empty, quiet);

    // The setup alone, without the steps.
    Setup setup;
    std::string error;
    Require(ReadSetup(stream.data(), stream.size(), setup, error) && setup.steps == 9000, "ReadSetup");
}

// A network match's fields come back; a replay written before them (its
// setup ends at the score) reads as an offline match, as before.
void NetworkFields()
{
    Setup online = MakeSetup(2, 1);
    online.online = 1;
    online.set_tweaks = 0x80;
    online.sim_profile = 3;
    online.series = 1'795'000'123'456LL;
    online.series_game = 2;
    const auto match = MakeMatch(online, 200, 1, 5);
    const Bytes stream = Record(online, match);
    Setup read;
    std::string error;
    Require(ReadSetup(stream.data(), stream.size(), read, error), "a network match's setup reads: " + error);
    Require(read.online == 1 && read.set_tweaks == 0x80 && read.sim_profile == 3
                && read.series == 1'795'000'123'456LL && read.series_game == 2,
            "a network match's fields come back");
    Replay(stream, online, match);

    // The same stream as an older build wrote it: the setup without the
    // network fields and what came after them (the count of skips, none).
    constexpr std::size_t kNetworkFields = 1 + 4 + 4 + 8 + 4 + 2;
    Bytes older(stream.begin(), stream.end());
    const std::uint32_t setup_size = std::uint32_t(older[4]) | std::uint32_t(older[5]) << 8
        | std::uint32_t(older[6]) << 16 | std::uint32_t(older[7]) << 24;
    older.erase(older.begin() + 8 + setup_size - kNetworkFields, older.begin() + 8 + setup_size);
    const std::uint32_t shorter = setup_size - std::uint32_t(kNetworkFields);
    for (int byte = 0; byte < 4; ++byte) older[4 + byte] = std::uint8_t(shorter >> (8 * byte));
    Setup old;
    Require(ReadSetup(older.data(), older.size(), old, error), "an older replay's setup reads: " + error);
    Require(old.online == 0 && old.set_tweaks == 0 && old.sim_profile == 0 && old.series == 0
                && old.series_game == 0 && old.seed == online.seed && old.score[1] == online.score[1],
            "an older replay is an offline match with its own fields");
    StreamReader reader;
    Require(reader.Open(older.data(), older.size(), error), "an older replay opens: " + error);
}

// The parts of cutscenes skipped come back with the setup; a live stream's
// start names none (they go beside it, as they happen); a replay from before
// them has none.
void CutsceneSkips()
{
    Setup setup = MakeSetup(2, 1);
    setup.online = 1;
    setup.series_game = 2;
    setup.skip_count = 3;
    setup.skips[0] = {0, 150};
    setup.skips[1] = {2, 0};
    setup.skips[2] = {7, 65535};
    const auto match = MakeMatch(setup, 120, 1, 13);
    const Bytes stream = Record(setup, match);
    Setup read;
    std::string error;
    Require(ReadSetup(stream.data(), stream.size(), read, error), "a setup with skips reads: " + error);
    Require(read.skip_count == 3 && read.skips[0].section == 0 && read.skips[0].after == 150
                && read.skips[1].section == 2 && read.skips[1].after == 0 && read.skips[2].section == 7
                && read.skips[2].after == 65535 && read.series_game == 2,
            "the parts skipped come back, after the fields before them");
    Replay(stream, setup, match);

    StreamWriter writer;
    Require(writer.Begin(setup), "the writer takes a setup with skips");
    Bytes start;
    writer.StartBytes(start, true);
    Setup live;
    Require(ReadSetup(start.data(), start.size(), live, error) && live.skip_count == 0 && live.series_game == 2,
            "a live stream's start names no skips");

    // No more than kMaxSkips are kept.
    Setup many = setup;
    many.skip_count = std::uint16_t(kMaxSkips + 5);
    for (int n = 0; n < kMaxSkips; ++n) many.skips[n] = {std::uint16_t(n), std::uint16_t(n * 3)};
    const Bytes full = Record(many, MakeMatch(many, 30, 1, 2));
    Require(ReadSetup(full.data(), full.size(), read, error) && read.skip_count == kMaxSkips
                && read.skips[kMaxSkips - 1].section == kMaxSkips - 1,
            "at most kMaxSkips are kept");

    // A replay from before skips: its setup ends at the network fields.
    setup.skip_count = 0;
    const Bytes none = Record(setup, match);
    Bytes older(none.begin(), none.end());
    const std::uint32_t setup_size = std::uint32_t(older[4]) | std::uint32_t(older[5]) << 8
        | std::uint32_t(older[6]) << 16 | std::uint32_t(older[7]) << 24;
    older.erase(older.begin() + 8 + setup_size - 2, older.begin() + 8 + setup_size);
    for (int byte = 0; byte < 4; ++byte) older[4 + byte] = std::uint8_t((setup_size - 2) >> (8 * byte));
    Require(ReadSetup(older.data(), older.size(), read, error) && read.skip_count == 0 && read.series_game == 2,
            "a replay from before skips has none: " + error);
    Replay(older, setup, match);
}

// A network match's live stream: written as it is played (its start as the
// match begins, then each whole step), read while it still arrives.
void LiveStreams()
{
    const Setup setup = MakeSetup(2, 1);
    const auto packets = MakeMatch(setup, 1200, 1000, 41);
    StreamWriter writer;
    Require(writer.Begin(setup), "the writer takes the setup");
    Bytes start;
    writer.StartBytes(start);
    std::vector<std::size_t> boundaries{0}; // the steps' bytes after each whole step
    for (std::size_t at = 0; at < packets.size(); at += 2)
    {
        for (std::size_t machine = 0; machine < 2; ++machine)
        {
            const Packet& packet = packets[at + machine];
            writer.Header(packet.machine, packet.remap, packet.checksum, packet.frame, packet.seed,
                          unsigned(packet.events.size()), unsigned(packet.data.size()));
            for (const auto& event : packet.events) writer.Event(event.data(), unsigned(event.size()));
            writer.Data(packet.data.data(), unsigned(packet.data.size()));
            for (std::size_t player = 0; player < packet.records.size(); ++player)
                writer.Record(packet.machine, int(player), packet.records[player].data(), packet.connected[player]);
        }
        writer.Settle();
        Require(writer.StepBytes().size() >= boundaries.back(), "the steps only grow");
        boundaries.push_back(writer.StepBytes().size());
    }
    Bytes live = start;
    live.insert(live.end(), writer.StepBytes().begin(), writer.StepBytes().end());
    const Bytes finished = writer.Finish();
    Setup final_setup;
    std::string error;
    Require(ReadSetup(finished.data(), finished.size(), final_setup, error) && final_setup.steps == 1200,
            "the finished stream counts its steps");
    const std::size_t final_start = finished.size() - (boundaries.back());
    Require(std::equal(finished.begin() + std::ptrdiff_t(final_start), finished.end(), live.begin() + std::ptrdiff_t(start.size()),
                       live.end()),
            "the live stream's steps are the finished stream's");
    Setup live_setup;
    Require(ReadSetup(live.data(), live.size(), live_setup, error) && live_setup.steps == 0
                && live_setup.stadium == setup.stadium,
            "the live stream starts with the setup as the match began");

    // Read while it arrives, in pieces cut anywhere: never into a step that
    // has not arrived whole.
    std::mt19937 random(7);
    StreamReader reader;
    std::size_t have = start.size() + 1 + random() % 40;
    Require(reader.Open(live.data(), have, error), "the start opens the live stream: " + error);
    std::size_t read_steps = 0;
    std::size_t packet = 0;
    while (read_steps < 1200)
    {
        const int ahead = reader.StepsAhead(5000);
        std::size_t whole = 0;
        while (whole + read_steps < 1200 && start.size() + boundaries[read_steps + whole + 1] <= have) ++whole;
        Require(std::size_t(ahead) == whole, "the steps ahead are those that arrived whole");
        for (int step = 0; step < ahead; ++step)
            for (int machine = 0; machine < 2; ++machine, ++packet)
            {
                PacketHeader header;
                Require(reader.Header(machine, header) && header.frame == packets[packet].frame,
                        "a live step reads: " + reader.Error());
                std::uint8_t data[300];
                unsigned size = 0;
                for (std::size_t event = 0; event < header.events; ++event) Require(reader.Event(data, size), "an event");
                Require(reader.Data(data, header.data_size), "the network data");
                std::uint8_t record[16], connected = 0;
                Require(reader.Record(machine, 0, record, connected)
                            && std::memcmp(record, packets[packet].records[0].data(), 16) == 0,
                        "the record");
            }
        read_steps += std::size_t(ahead);
        Require(reader.Error().empty(), "no step was read before it arrived");
        if (have == live.size()) break;
        have = std::min(live.size(), have + 1 + random() % 300);
        reader.Grow(have);
    }
    Require(read_steps == 1200 && reader.StepsAhead(1) == 0, "the whole live stream reads");

    // The ring the module hands it over in.
    LiveRing<1024> ring;
    std::uint8_t payload[600];
    for (std::size_t n = 0; n < sizeof payload; ++n) payload[n] = std::uint8_t(n * 7);
    std::vector<std::uint8_t> out(4096);
    std::uint32_t sent = 0, received = 0;
    for (int round = 0; round < 200; ++round)
    {
        const std::uint32_t size = std::uint32_t(random() % 400);
        if (ring.Put(kLiveData, sent, payload, size)) ++sent;
        else Require(sent > received, "only a ring holding messages refuses one");
        if (random() % 2)
        {
            const std::size_t capacity = kLiveHeaderSize + std::size_t(random() % 700);
            const std::size_t taken = ring.Take(out.data(), capacity);
            Require(taken <= capacity, "Take stays in its buffer");
            std::size_t at = 0;
            LiveMessage message;
            while (NextLiveMessage(out.data(), taken, at, message))
            {
                Require(message.kind == kLiveData && message.stream == received++, "messages come in order");
                Require(std::memcmp(message.data, payload, message.size) == 0, "a message's bytes come back");
            }
            Require(at == taken, "Take copies whole messages only");
        }
    }
    Require(!ring.Put(kLiveData, 0, payload, std::uint32_t(kLiveMostData + 1)), "a message over the most is refused");
}

void Refusals()
{
    const Setup setup = MakeSetup(1, 2);
    const auto match = MakeMatch(setup, 400, 0, 4);
    const Bytes stream = Record(setup, match);
    std::string error;
    StreamReader reader;
    // Every cut ends the reader without reading past the end.
    for (std::size_t cut = 0; cut < stream.size(); cut += 7)
    {
        if (!reader.Open(stream.data(), cut, error)) continue;
        PacketHeader header;
        std::uint8_t record[16], data[kMaxDataSize], connected;
        unsigned size;
        while (reader.Header(0, header))
        {
            for (unsigned event = 0; event < header.events; ++event) reader.Event(data, size);
            reader.Data(data, header.data_size);
            for (int player = 0; player < 2; ++player) reader.Record(0, player, record, connected);
        }
    }
    Require(true, "cut streams stop");
    // Damaged bytes anywhere: refused or read to an end, never past it.
    std::mt19937 random(5);
    for (int trial = 0; trial < 2000; ++trial)
    {
        Bytes damaged = stream;
        for (int n = 0; n < 4; ++n) damaged[random() % damaged.size()] = std::uint8_t(random());
        if (!reader.Open(damaged.data(), damaged.size(), error)) continue;
        PacketHeader header;
        std::uint8_t record[16], data[kMaxDataSize], connected;
        unsigned size;
        while (reader.Header(0, header))
        {
            for (unsigned event = 0; event < header.events; ++event) reader.Event(data, size);
            reader.Data(data, header.data_size);
            for (int player = 0; player < 2; ++player) reader.Record(0, player, record, connected);
        }
    }
    Require(true, "damaged streams stop");
    // Out of order reads are errors.
    Require(reader.Open(stream.data(), stream.size(), error), "reopen");
    std::uint8_t record[16], connected;
    Require(!reader.Record(0, 0, record, connected) && !reader.Error().empty(), "a record before its header");
    // An unknown version is refused with a reason.
    Bytes future = stream;
    future[0] = 99;
    Require(!reader.Open(future.data(), future.size(), error) && !error.empty(), "a newer stream version");
    // The writer refuses counts out of range and packets out of order.
    StreamWriter writer;
    Setup wrong = setup;
    wrong.machine_count = 5;
    Require(!writer.Begin(wrong), "five machines");
    Require(writer.Begin(setup), "begin");
    const std::uint8_t bytes[16] = {};
    writer.Header(0, 0, 0, 0, 0, 0, 0);
    writer.Record(0, 1, bytes, 1); // player 0 missing
    Require(writer.Broken(), "a record out of order breaks the stream");
    Setup kept;
    const Bytes partial = writer.Finish();
    Require(ReadSetup(partial.data(), partial.size(), kept, error) && kept.steps == 0, "what came before is kept");
}

void Files()
{
    const Setup setup = MakeSetup(1, 4);
    const Bytes stream = Record(setup, MakeMatch(setup, 9000, 0, 6));
    FileInfo info;
    info.build = "1.2.0+gabcdef";
    info.game_id = "R4QP01";
    info.revision = 2;
    info.created = 1795302000;
    Setup finished = setup;
    finished.steps = 9000;
    finished.home_team = 4;
    finished.away_team = 11;
    finished.stadium = 7;
    finished.score[0] = 3;
    finished.score[1] = 1;
    finished.training = 1;
    Summarize(finished, info);
    const auto file = EncodeFile(info, stream.data(), stream.size());
    std::printf("offline 9000 steps packed: %zu bytes\n", file.size());
    FileInfo read;
    std::vector<std::uint8_t> unpacked;
    std::string error;
    Require(DecodeFile(file.data(), file.size(), read, unpacked, error), "the file decodes: " + error);
    Require(read.build == info.build && read.game_id == "R4QP01" && read.revision == 2 && read.created == info.created,
            "the file's header comes back");
    Require(unpacked.size() == stream.size() && std::memcmp(unpacked.data(), stream.data(), stream.size()) == 0,
            "the stream comes back");
    Require(read.steps == 9000 && read.home_team == 4 && read.away_team == 11 && read.stadium == 7
                && read.score[0] == 3 && read.score[1] == 1 && read.training,
            "the match's summary comes back");
    Require(MatchLength(9000) == "3:00" && MatchLength(49) == "0:00" && MatchLength(50 * 61) == "1:01"
                && MatchLength(50 * 3600) == "60:00",
            "a match's length");
    Require(!DecodeFileInfo(file.data(), 40, read, error) && !error.empty(), "a cut header is refused");
    // A header without the summary (shorter), and one with more than this
    // build knows (longer): both read, the unknown bytes skipped.
    {
        const std::size_t header = std::size_t(file[10]) | (std::size_t(file[11]) << 8);
        const std::size_t summary_at = 12 + header - 15;
        std::vector<std::uint8_t> shorter(file);
        shorter.erase(shorter.begin() + std::ptrdiff_t(summary_at), shorter.begin() + std::ptrdiff_t(summary_at + 15));
        shorter[10] = std::uint8_t(header - 15);
        FileInfo plain;
        Require(DecodeFile(shorter.data(), shorter.size(), plain, unpacked, error) && plain.steps == 0
                    && plain.home_team == -1 && !plain.training && unpacked.size() == stream.size(),
                "a header without the summary reads: " + error);
        std::vector<std::uint8_t> longer(file);
        longer.insert(longer.begin() + std::ptrdiff_t(12 + header), {1, 2, 3, 4, 5});
        longer[10] = std::uint8_t(header + 5);
        FileInfo more;
        Require(DecodeFile(longer.data(), longer.size(), more, unpacked, error) && more.score[0] == 3
                    && unpacked.size() == stream.size(),
                "a longer header reads: " + error);
    }
    for (std::size_t cut = 0; cut < file.size(); cut += 101)
        Require(!DecodeFile(file.data(), cut, read, unpacked, error), "a cut file is refused");
    std::vector<std::uint8_t> other(file);
    other[0] = 'X';
    Require(!DecodeFile(other.data(), other.size(), read, unpacked, error), "not a replay");
    std::vector<std::uint8_t> damaged(file);
    damaged[damaged.size() / 2] ^= 0x55;
    Require(!DecodeFile(damaged.data(), damaged.size(), read, unpacked, error), "damaged packed data");

    Require(ReplayFileName(info.created).rfind("replay_", 0) == 0
                && ReplayFileName(info.created).size() == std::string("replay_20261122-000000.msc").size(),
            "the file name");

    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / ("recharged-replay-test-" + std::to_string(std::random_device{}()));
    fs::remove_all(folder);
    for (int n = 0; n < 5; ++n)
    {
        const auto path = WriteReplay(folder, "replay_2026010" + std::to_string(n) + "-120000.msc", file, error);
        Require(!path.empty(), "a replay is written: " + error);
    }
    const auto again = WriteReplay(folder, "replay_20260104-120000.msc", file, error);
    Require(again.filename() == "replay_20260104-120000-2.msc", "a name in use gets a number");
    std::vector<std::uint8_t> bytes;
    Require(ReadReplayFile(again, bytes, error) && bytes == file, "the written file reads back");
    Require(PruneReplays(folder, 3) == 3, "the oldest three of six go");
    Require(!fs::exists(folder / "replay_20260100-120000.msc") && fs::exists(folder / "replay_20260104-120000.msc"),
            "the newest stay");
    Require(PruneReplays(folder, 0) == 0, "keep 0 keeps all");
    FileInfo header;
    Require(ReadReplayInfo(again, header, error) && header.game_id == "R4QP01" && header.score[0] == 3
                && header.steps == 9000,
            "a file's header alone reads: " + error);
    // Listings: the newest first, any .msc in any case; something else is no replay.
    std::ofstream(folder / "Final.MSC", std::ios::binary) << "not a replay";
    std::ofstream(folder / "notes.txt", std::ios::binary) << "notes";
    fs::last_write_time(folder / "Final.MSC", fs::last_write_time(again) + std::chrono::hours(1));
    std::size_t total = 0;
    const auto listed = ListReplays(folder, 2, &total);
    Require(total == 4 && listed.size() == 2, "the folder's replays are listed");
    Require(listed[0].path.filename() == "Final.MSC" && !listed[0].error.empty(), "a broken replay lists its error");
    Require(listed[1].error.empty() && listed[1].info.steps == 9000, "a replay lists its summary");
    Require(IsReplayFile("a/b.msc") && IsReplayFile("B.MsC") && !IsReplayFile("b.msc.txt") && !IsReplayFile("msc"),
            "replay files by their extension");
    Require(ListReplays(folder / "missing", 5, &total).empty() && total == 0, "a missing folder lists none");
    fs::remove_all(folder);
}
} // namespace

int main()
{
    try
    {
        RoundTrips();
        NetworkFields();
        CutsceneSkips();
        LiveStreams();
        Refusals();
        Files();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "match replay test failed: %s\n", error.what());
        return 1;
    }
    std::printf("match replays: %d checks passed\n", sChecks);
    return 0;
}
