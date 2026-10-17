#include "disc.h"
#include "path.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

#include <nod.h>

namespace mscharged
{
namespace
{
using Handle = std::unique_ptr<NodHandle, decltype(&nod_free)>;

void Check(NodResult result, const char* context)
{
    if (result != NOD_RESULT_OK)
    {
        const char* error = nod_error_message();
        throw std::runtime_error(std::string(context) + ": " + (error ? error : "Disc reader failed"));
    }
}

uint32_t CountFile(uint32_t index, NodNodeKind kind, const char*, uint32_t, void* data)
{
    if (kind == NOD_NODE_KIND_FILE)
        ++*static_cast<uint32_t*>(data);
    return index + 1;
}
bool IsChargedHeader(const NodDiscHeader& header)
{
    constexpr uint8_t wii_magic[] = {0x5D, 0x1C, 0x9E, 0xA3};
    const std::string game_id(header.game_id, sizeof(header.game_id));
    return std::equal(std::begin(wii_magic), std::end(wii_magic), header.wii_magic)
        && game_id.substr(0, 3) == "R4Q" && game_id.substr(4) == "01";
}

std::string HeaderTitle(const NodDiscHeader& header)
{
    return std::string(header.game_title, std::find(std::begin(header.game_title), std::end(header.game_title), '\0'));
}

Handle OpenChargedDisc(const std::filesystem::path& path,
                       NodDiscHeader& header, NodDiscMeta& metadata)
{
    NodHandle* raw_disc = nullptr;
    Check(nod_disc_open(PathUtf8(path).c_str(), nullptr, &raw_disc), "Cannot open disc");
    Handle disc(raw_disc, nod_free);
    Check(nod_disc_header(disc.get(), &header), "Cannot read disc header");
    Check(nod_disc_meta(disc.get(), &metadata), "Cannot read disc metadata");

    if (!IsChargedHeader(header))
        throw std::runtime_error("Expected a Mario Strikers Charged Wii image (R4Q?01).");
    if (metadata.format != NOD_FORMAT_ISO && metadata.format != NOD_FORMAT_RVZ)
        throw std::runtime_error("This build accepts ISO and RVZ disc images.");

    return disc;
}

Handle OpenGamePartition(NodHandle* disc)
{
    NodHandle* raw_partition = nullptr;
    Check(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, nullptr, &raw_partition),
          "Cannot open game data partition");
    return Handle(raw_partition, nod_free);
}
}

namespace
{
// Opens the game partition of an open Charged disc and counts its files.
DiscInfo ReadChargedInfo(NodHandle* disc, const NodDiscHeader& header, const NodDiscMeta& metadata)
{
    auto partition = OpenGamePartition(disc);
    uint32_t file_count = 0;
    nod_partition_iterate_fst(partition.get(), CountFile, &file_count);
    if (const char* error = nod_error_message())
        throw std::runtime_error(std::string("Cannot read game file table: ") + error);
    if (file_count == 0)
        throw std::runtime_error("The game data partition has no files.");

    return {metadata.format == NOD_FORMAT_RVZ ? "RVZ" : "ISO",
            std::string(header.game_id, sizeof(header.game_id)), HeaderTitle(header), header.disc_version,
            file_count};
}
}

DiscInfo InspectDisc(const std::filesystem::path& path)
{
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    auto disc = OpenChargedDisc(path, header, metadata);
    return ReadChargedInfo(disc.get(), header, metadata);
}

DiscProbe ProbeDisc(const std::filesystem::path& path)
{
    DiscProbe probe;
    const auto fail = [&](DiscProbe::Kind kind, const char* context) {
        const char* error = nod_error_message();
        probe.kind = kind;
        probe.error = std::string(context) + ": " + (error ? error : "Disc reader failed");
        return probe;
    };
    NodHandle* raw_disc = nullptr;
    const NodResult opened = nod_disc_open(PathUtf8(path).c_str(), nullptr, &raw_disc);
    Handle disc(raw_disc, nod_free);
    // A format error: the file is no disc image the reader knows (an ISO of
    // something else). Other errors come from an image it could not read.
    if (opened != NOD_RESULT_OK)
        return fail(opened == NOD_RESULT_ERR_FORMAT ? DiscProbe::Kind::NotDisc : DiscProbe::Kind::Unreadable,
                    "Cannot open disc");
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    if (nod_disc_header(disc.get(), &header) != NOD_RESULT_OK)
        return fail(DiscProbe::Kind::Unreadable, "Cannot read disc header");
    if (nod_disc_meta(disc.get(), &metadata) != NOD_RESULT_OK)
        return fail(DiscProbe::Kind::Unreadable, "Cannot read disc metadata");
    if (!IsChargedHeader(header))
    {
        probe.kind = DiscProbe::Kind::OtherGame;
        probe.info.game_id = std::string(header.game_id, sizeof(header.game_id));
        probe.info.title = HeaderTitle(header);
        probe.info.revision = header.disc_version;
        probe.info.file_count = 0;
        return probe;
    }
    if (metadata.format != NOD_FORMAT_ISO && metadata.format != NOD_FORMAT_RVZ)
    {
        probe.error = "This build accepts ISO and RVZ disc images.";
        return probe;
    }
    try
    {
        probe.info = ReadChargedInfo(disc.get(), header, metadata);
        probe.kind = DiscProbe::Kind::Charged;
    }
    catch (const std::exception& error) { probe.error = error.what(); }
    return probe;
}

std::vector<std::uint8_t> ReadDiscFile(const std::filesystem::path& path, const std::string& file,
                                       std::size_t max_size)
{
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    auto disc = OpenChargedDisc(path, header, metadata);
    auto partition = OpenGamePartition(disc.get());
    NodNodeKind kind{};
    uint32_t length = 0;
    const uint32_t index = nod_partition_find_file(partition.get(), file.c_str(), &kind, &length);
    if (index == NOD_FST_STOP || kind != NOD_NODE_KIND_FILE)
        throw std::runtime_error("The disc has no " + file + ".");
    if (length > max_size)
        throw std::runtime_error("The disc's " + file + " is unexpectedly large.");
    NodHandle* raw_file = nullptr;
    Check(nod_partition_open_file(partition.get(), index, &raw_file), ("Cannot open " + file).c_str());
    Handle handle(raw_file, nod_free);
    std::vector<std::uint8_t> data(length);
    std::size_t done = 0;
    while (done < data.size())
    {
        const int64_t read = nod_read(handle.get(), data.data() + done, data.size() - done);
        if (read < 0) Check(NOD_RESULT_ERR_IO, ("Cannot read " + file).c_str());
        if (read <= 0) throw std::runtime_error("The disc's " + file + " ends early.");
        done += std::size_t(read);
    }
    return data;
}

const char* DiscRegionName(const std::string& game_id)
{
    if (game_id.size() != 6) return "Unknown region";
    switch (game_id[3])
    {
    case 'E': return "USA";
    case 'P': return "Europe";
    case 'J': return "Japan";
    case 'K': return "Korea";
    default: return "Unknown region";
    }
}

std::vector<std::uint8_t> ReadDiscMainDol(const std::filesystem::path& path)
{
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    auto disc = OpenChargedDisc(path, header, metadata);
    auto partition = OpenGamePartition(disc.get());
    NodPartitionMeta partition_metadata{};
    Check(nod_partition_meta(partition.get(), &partition_metadata), "Cannot read the game program");
    const auto& dol = partition_metadata.raw_dol;
    if (!dol.data || dol.size < 0x100)
        throw std::runtime_error("The game data partition has no main.dol.");
    // Copy while the partition handle still owns the borrowed bytes.
    return std::vector<std::uint8_t>(dol.data, dol.data + dol.size);
}

std::uint64_t ReadDiscTitleId(const std::filesystem::path& path)
{
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    auto disc = OpenChargedDisc(path, header, metadata);
    auto partition = OpenGamePartition(disc.get());
    NodPartitionMeta partition_metadata{};
    Check(nod_partition_meta(partition.get(), &partition_metadata),
          "Cannot read game title metadata");
    const auto& tmd = partition_metadata.raw_tmd;
    // Pinned nod's SignedHeader/TmdHeader are fixed RSA-2048 records, with
    // a 0x140-byte signature header and an eight-byte BE identity at 0x18c.
    // Retain the partition handle while using its borrowed metadata bytes.
    constexpr unsigned char rsa2048[] = {0x00, 0x01, 0x00, 0x01};
    if (!tmd.data || tmd.size < 0x1e4)
        throw std::runtime_error("The game data partition has no complete TMD header.");
    if (!std::equal(std::begin(rsa2048), std::end(rsa2048), tmd.data))
        throw std::runtime_error("Unsupported game TMD signature layout.");
    std::uint64_t title = 0;
    for (unsigned i = 0; i < 8; ++i)
        title = (title << 8) | tmd.data[0x18c + i];
    if (!title)
        throw std::runtime_error("The game TMD has no title identity.");
    return title;
}

std::uint16_t ReadDiscTitleGroupId(const std::filesystem::path& path)
{
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    auto disc = OpenChargedDisc(path, header, metadata);
    auto partition = OpenGamePartition(disc.get());
    NodPartitionMeta partition_metadata{};
    Check(nod_partition_meta(partition.get(), &partition_metadata),
          "Cannot read game title metadata");
    const auto& tmd = partition_metadata.raw_tmd;
    constexpr unsigned char rsa2048[] = {0x00, 0x01, 0x00, 0x01};
    if (!tmd.data || tmd.size < 0x1e4)
        throw std::runtime_error("The game data partition has no complete TMD header.");
    if (!std::equal(std::begin(rsa2048), std::end(rsa2048), tmd.data))
        throw std::runtime_error("Unsupported game TMD signature layout.");
    // Pinned nod's fixed RSA-2048 TmdHeader places group_id at0x198.
    // Read the BE scalar while the owning partition handle is alive.
    return (std::uint16_t(tmd.data[0x198]) << 8) | tmd.data[0x199];
}
}
