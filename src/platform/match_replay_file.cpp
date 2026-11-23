#include "platform/match_replay_file.h"

#include "platform/json.h"

#include "platform/path.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace mscharged::replay
{
namespace
{
constexpr char kMagic[8] = {'M', 'S', 'C', 'R', 'P', 'L', 'Y', 0x1A};
constexpr std::size_t kFixedHeader = 8 + 2 + 2; // magic, version, header size
// The header after the fixed part, without the build's text and the summary.
constexpr std::size_t kHeaderBase = 1 + 6 + 1 + 8 + 4 + 4;
constexpr std::size_t kSummarySize = 4 + 2 + 2 + 2 + 2 + 2 + 1;
// The most a header takes: a reader of the header alone reads this much.
constexpr std::size_t kMostHeader = kFixedHeader + kHeaderBase + 255 + kSummarySize + 256;

void Put16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(std::uint8_t(value));
    out.push_back(std::uint8_t(value >> 8));
}
void Put32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) out.push_back(std::uint8_t(value >> shift));
}
void Put64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) out.push_back(std::uint8_t(value >> shift));
}
std::uint64_t Get(const std::uint8_t* data, int bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value |= std::uint64_t(data[i]) << (8 * i);
    return value;
}

struct Parsed
{
    FileInfo info;
    std::uint32_t stream_size = 0;
    std::uint32_t packed_size = 0;
    std::size_t packed_at = 0;
};

// `header_only`: `data` may stop after the header.
bool Parse(const std::uint8_t* data, std::size_t size, Parsed& parsed, std::string& error, bool header_only = false)
{
    if (!data || size < kFixedHeader || std::memcmp(data, kMagic, sizeof kMagic) != 0)
    {
        error = "This is not a Recharged replay.";
        return false;
    }
    const auto version = std::uint16_t(Get(data + 8, 2));
    const auto header = std::size_t(Get(data + 10, 2));
    if (version != kFileVersion)
    {
        error = "The replay was made by a newer Recharged (file version " + std::to_string(version) + ").";
        return false;
    }
    if (header < kHeaderBase || size - kFixedHeader < header)
    {
        error = "The replay's header is damaged.";
        return false;
    }
    const std::uint8_t* at = data + kFixedHeader;
    const std::size_t build = at[0];
    if (kHeaderBase + build > header)
    {
        error = "The replay's header is damaged.";
        return false;
    }
    parsed.info.build.assign(reinterpret_cast<const char*>(at + 1), build);
    at += 1 + build;
    parsed.info.game_id.assign(reinterpret_cast<const char*>(at), 6);
    parsed.info.revision = at[6];
    parsed.info.created = std::int64_t(Get(at + 7, 8));
    parsed.stream_size = std::uint32_t(Get(at + 15, 4));
    parsed.packed_size = std::uint32_t(Get(at + 19, 4));
    parsed.packed_at = kFixedHeader + header;
    if (kHeaderBase + build + kSummarySize <= header)
    {
        const std::uint8_t* summary = at + 23;
        FileInfo& info = parsed.info;
        info.steps = std::uint32_t(Get(summary, 4));
        info.home_team = std::int16_t(Get(summary + 4, 2));
        info.away_team = std::int16_t(Get(summary + 6, 2));
        info.stadium = std::int16_t(Get(summary + 8, 2));
        info.score[0] = int(Get(summary + 10, 2));
        info.score[1] = int(Get(summary + 12, 2));
        info.training = (summary[14] & 1) != 0;
        info.network = (summary[14] & 2) != 0;
    }
    if (parsed.stream_size > kMaxStreamSize || (!header_only && parsed.packed_size > size - parsed.packed_at))
    {
        error = "The replay is damaged or cut off.";
        return false;
    }
    return true;
}
} // namespace

std::vector<std::uint8_t> EncodeFile(const FileInfo& info, const std::uint8_t* stream, std::size_t size)
{
    if (size > kMaxStreamSize) throw std::runtime_error("The replay is too large.");
    uLongf packed_size = compressBound(uLong(size));
    std::vector<std::uint8_t> packed(packed_size);
    if (compress2(packed.data(), &packed_size, stream, uLong(size), Z_BEST_COMPRESSION) != Z_OK)
        throw std::runtime_error("The replay could not be compressed.");
    packed.resize(packed_size);
    const std::string build = info.build.substr(0, 255);
    std::string game_id = info.game_id.substr(0, 6);
    game_id.resize(6, ' ');
    std::vector<std::uint8_t> out(kMagic, kMagic + sizeof kMagic);
    Put16(out, kFileVersion);
    Put16(out, std::uint16_t(kHeaderBase + build.size() + kSummarySize));
    out.push_back(std::uint8_t(build.size()));
    out.insert(out.end(), build.begin(), build.end());
    out.insert(out.end(), game_id.begin(), game_id.end());
    out.push_back(std::uint8_t(std::clamp(info.revision, 0, 255)));
    Put64(out, std::uint64_t(info.created));
    Put32(out, std::uint32_t(size));
    Put32(out, std::uint32_t(packed.size()));
    const auto i16 = [](int value) { return std::uint16_t(std::int16_t(std::clamp(value, -32768, 32767))); };
    Put32(out, info.steps);
    Put16(out, i16(info.home_team));
    Put16(out, i16(info.away_team));
    Put16(out, i16(info.stadium));
    Put16(out, std::uint16_t(std::clamp(info.score[0], 0, 65535)));
    Put16(out, std::uint16_t(std::clamp(info.score[1], 0, 65535)));
    out.push_back(std::uint8_t((info.training ? 1 : 0) | (info.network ? 2 : 0)));
    out.insert(out.end(), packed.begin(), packed.end());
    return out;
}

void Summarize(const Setup& setup, FileInfo& info)
{
    info.steps = setup.steps;
    info.home_team = setup.home_team;
    info.away_team = setup.away_team;
    info.stadium = setup.stadium;
    info.score[0] = setup.score[0];
    info.score[1] = setup.score[1];
    info.training = setup.training != 0;
    info.network = setup.machine_count > 1 || setup.online != 0;
}

std::string MatchLength(std::uint32_t steps)
{
    const std::uint32_t seconds = steps / kStepsPerSecond;
    char text[24];
    std::snprintf(text, sizeof text, "%u:%02u", unsigned(seconds / 60), unsigned(seconds % 60));
    return text;
}

bool IsReplayFile(const std::filesystem::path& path)
{
    std::string extension = PathUtf8(path.extension());
    for (auto& c : extension) c = char(std::tolower(static_cast<unsigned char>(c)));
    return extension == kFileExtension;
}

bool DecodeFileInfo(const std::uint8_t* data, std::size_t size, FileInfo& info, std::string& error)
{
    Parsed parsed;
    if (!Parse(data, size, parsed, error)) return false;
    info = parsed.info;
    return true;
}

bool DecodeFile(const std::uint8_t* data, std::size_t size, FileInfo& info, std::vector<std::uint8_t>& stream,
                std::string& error)
{
    Parsed parsed;
    if (!Parse(data, size, parsed, error)) return false;
    stream.assign(parsed.stream_size, 0);
    uLongf unpacked = parsed.stream_size;
    const int result = uncompress(stream.data(), &unpacked, data + parsed.packed_at, uLong(parsed.packed_size));
    if (result != Z_OK || unpacked != parsed.stream_size)
    {
        error = "The replay is damaged.";
        stream.clear();
        return false;
    }
    info = parsed.info;
    return true;
}

bool ReadReplayFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, std::string& error)
{
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code)
    {
        error = "Cannot read " + PathUtf8(path) + ": " + code.message();
        return false;
    }
    if (size > kMaxStreamSize + 4096)
    {
        error = PathUtf8(path) + " is too large for a replay.";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    bytes.assign(std::size_t(size), 0);
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)))
    {
        error = "Cannot read " + PathUtf8(path) + ".";
        bytes.clear();
        return false;
    }
    return true;
}

bool ReadReplayInfo(const std::filesystem::path& path, FileInfo& info, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "Cannot read " + PathUtf8(path) + ".";
        return false;
    }
    std::vector<std::uint8_t> bytes(kMostHeader);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    bytes.resize(std::size_t(file.gcount()));
    Parsed parsed;
    if (!Parse(bytes.data(), bytes.size(), parsed, error, true)) return false;
    info = parsed.info;
    return true;
}

std::string ReplayFileName(std::int64_t created)
{
    const std::time_t time = std::time_t(created);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char name[64];
    std::strftime(name, sizeof name, "replay_%Y%m%d-%H%M%S", &local);
    return std::string(name) + kFileExtension;
}

std::filesystem::path ReplayFolder(const std::filesystem::path& program_folder)
{
    return program_folder / "replays";
}

std::filesystem::path WriteReplay(const std::filesystem::path& folder, const std::string& name,
                                  const std::vector<std::uint8_t>& bytes, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code code;
    fs::create_directories(folder, code);
    if (code)
    {
        error = "Cannot make " + PathUtf8(folder) + ": " + code.message();
        return {};
    }
    const fs::path base = PathFromUtf8(name);
    fs::path path = folder / base;
    for (int n = 2; fs::exists(path, code) && n < 1000; ++n)
        path = folder / PathFromUtf8(PathUtf8(base.stem()) + "-" + std::to_string(n) + kFileExtension);
    const fs::path temporary = folder / PathFromUtf8("." + PathUtf8(path.filename()) + ".writing");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))
            || !file.flush())
        {
            error = "Cannot write " + PathUtf8(path) + ".";
            file.close();
            fs::remove(temporary, code);
            return {};
        }
    }
    fs::rename(temporary, path, code);
    if (code)
    {
        error = "Cannot write " + PathUtf8(path) + ": " + code.message();
        fs::remove(temporary, code);
        return {};
    }
    return path;
}

int PruneReplays(const std::filesystem::path& folder, int keep)
{
    namespace fs = std::filesystem;
    if (keep <= 0) return 0;
    std::error_code code;
    std::vector<fs::path> replays;
    for (fs::directory_iterator it(folder, code), end; !code && it != end; it.increment(code))
    {
        const auto name = PathUtf8(it->path().filename());
        if (it->is_regular_file(code) && name.rfind("replay_", 0) == 0 && IsReplayFile(it->path()))
            replays.push_back(it->path());
    }
    if (int(replays.size()) <= keep) return 0;
    // Their names begin with the time they were made: the oldest sort first.
    std::sort(replays.begin(), replays.end());
    int removed = 0;
    for (std::size_t n = 0; n + std::size_t(keep) < replays.size(); ++n)
        if (fs::remove(replays[n], code)) ++removed;
    return removed;
}

std::vector<Listing> ListReplays(const std::filesystem::path& folder, std::size_t limit, std::size_t* total)
{
    namespace fs = std::filesystem;
    std::error_code code;
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    for (fs::directory_iterator it(folder, code), end; !code && it != end; it.increment(code))
        if (it->is_regular_file(code) && IsReplayFile(it->path()))
        {
            const auto written = it->last_write_time(code);
            found.emplace_back(code ? fs::file_time_type::min() : written, it->path());
        }
    if (total) *total = found.size();
    // The newest first; the same time by name, the later first.
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    std::vector<Listing> listings;
    for (std::size_t n = 0; n < found.size() && n < limit; ++n)
    {
        Listing listing;
        listing.path = found[n].second;
        ReadReplayInfo(listing.path, listing.info, listing.error);
        listings.push_back(std::move(listing));
    }
    return listings;
}
bool ReadWatchTicket(const std::vector<std::uint8_t>& bytes, WatchTicket& ticket, std::string& error)
{
    static constexpr char magic[8] = {'M', 'S', 'C', 'R', 'P', 'L', 'Y', 0x1A};
    if (bytes.size() < 12 || std::memcmp(bytes.data(), magic, sizeof magic) != 0) return false;
    if ((bytes[8] | bytes[9] << 8) != 2) return false;
    const std::size_t size = std::size_t(bytes[10] | bytes[11] << 8);
    if (bytes.size() < 12 + size)
    {
        error = "The watch ticket is cut short.";
        return false;
    }
    try
    {
        const auto header =
            mscharged::platform::ParseJson(std::string_view(reinterpret_cast<const char*>(bytes.data() + 12), size));
        const auto match = header["match"].Integer();
        ticket.ticket = header["ticket"].Text();
        ticket.release = header["release"].Text();
        if (!match || *match <= 0 || ticket.ticket.empty() || ticket.ticket.size() > 256
            || ticket.ticket.find_first_not_of("0123456789abcdef") != std::string::npos || ticket.release.size() != 4)
        {
            error = "The watch ticket does not name a match.";
            return false;
        }
        ticket.match = *match;
        return true;
    }
    catch (const std::exception&)
    {
        error = "The watch ticket cannot be read.";
        return false;
    }
}
} // namespace mscharged::replay
