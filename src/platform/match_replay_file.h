#pragma once

// Match replays on disk: a .msc file in the replays folder beside the program
// holds one match (match_replay_stream.h), compressed, behind a short header
// that says which build recorded it and from which game image.
//
// Layout (little-endian): "MSCRPLY" 0x1A, u16 file version, u16 header size,
// then the header (u8 build length and the build, the 6-character game ID,
// u8 revision, i64 creation time in seconds since 1970, u32 stream size,
// u32 compressed size, then the match's summary: u32 steps, i16 home and away
// captain, i16 stadium, u16 home and away score, u8 flags, bit 0 Training, bit 1
// a network match),
// then the zlib stream. A reader skips header bytes it does not know.

#include "platform/match_replay_stream.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mscharged::replay
{
inline constexpr std::uint16_t kFileVersion = 1;
// A replay larger than this is refused (an hour of four players is far below).
inline constexpr std::size_t kMaxStreamSize = 64u << 20;
inline constexpr const char* kFileExtension = ".msc";

struct FileInfo
{
    std::string build;   // the build that recorded it
    std::string game_id; // R4QE01, R4QP01, ...
    int revision = 0;
    std::int64_t created = 0; // seconds since 1970
    // What the match was, for listings without inflating the stream.
    std::uint32_t steps = 0;
    int home_team = -1; // eTeamID
    int away_team = -1;
    int stadium = -1; // eStadiumID
    int score[2] = {};
    bool training = false;
    bool network = false; // online or LAN: every machine's inputs
};

// The game's fixed steps a second.
inline constexpr std::uint32_t kStepsPerSecond = 50;

// The summary part of `info` from the stream's setup.
void Summarize(const Setup& setup, FileInfo& info);
// A match's length as m:ss.
std::string MatchLength(std::uint32_t steps);
// Whether `path` names a replay file (.msc, in any case).
bool IsReplayFile(const std::filesystem::path& path);

// The file's bytes; throws std::runtime_error when the stream cannot be packed.
std::vector<std::uint8_t> EncodeFile(const FileInfo& info, const std::uint8_t* stream, std::size_t size);
// The header alone (cheap: no inflating); false with `error` set.
bool DecodeFileInfo(const std::uint8_t* data, std::size_t size, FileInfo& info, std::string& error);
bool DecodeFile(const std::uint8_t* data, std::size_t size, FileInfo& info, std::vector<std::uint8_t>& stream,
                std::string& error);
// Reads a file whole (at most the largest packed replay); false with `error` set.
bool ReadReplayFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, std::string& error);
// Reads only a file's header; false with `error` set.
bool ReadReplayInfo(const std::filesystem::path& path, FileInfo& info, std::string& error);

// replay_YYYYMMDD-HHMMSS.msc for a local time.
std::string ReplayFileName(std::int64_t created);
// The replays folder beside the program.
std::filesystem::path ReplayFolder(const std::filesystem::path& program_folder);
// Writes `bytes` as a new file in `folder` (made if needed) under `name`, or
// under name-2, name-3... when that exists; the written path, empty with
// `error` set on failure. The file appears whole or not at all.
std::filesystem::path WriteReplay(const std::filesystem::path& folder, const std::string& name,
                                  const std::vector<std::uint8_t>& bytes, std::string& error);
// Deletes the oldest replay_*.msc files beyond the newest `keep` (0 keeps all).
int PruneReplays(const std::filesystem::path& folder, int keep);

// A replay of a folder, with its header (or why it cannot be read).
struct Listing
{
    std::filesystem::path path;
    FileInfo info;
    std::string error;
};
// The newest `limit` replays of `folder` (by the time they were written),
// newest first; `total` gets how many the folder has.
std::vector<Listing> ListReplays(const std::filesystem::path& folder, std::size_t limit, std::size_t* total = nullptr);
} // namespace mscharged::replay
