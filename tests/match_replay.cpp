// Match replays: the compact stream round trip (what the game's recorder hands
// over comes back the same, in the same order), damaged streams and files are
// refused without reading past their end, and the .msc file's packing, names
// and pruning.
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
