#include "disc.h"
#include "path.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

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

// SHA-1 (FIPS 180-4) for the main.dol identity; not used for security.
std::string Sha1Hex(const unsigned char* data, std::size_t size)
{
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    std::vector<unsigned char> message(data, data + size);
    const std::uint64_t bits = std::uint64_t(size) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<unsigned char>(bits >> (i * 8)));
    for (std::size_t block = 0; block < message.size(); block += 64)
    {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = std::uint32_t(message[block + i * 4]) << 24 | std::uint32_t(message[block + i * 4 + 1]) << 16
                | std::uint32_t(message[block + i * 4 + 2]) << 8 | message[block + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const std::uint32_t word : h)
        for (int shift = 28; shift >= 0; shift -= 4) hex += digits[(word >> shift) & 0xf];
    return hex;
}

uint32_t CountFile(uint32_t index, NodNodeKind kind, const char*, uint32_t, void* data)
{
    if (kind == NOD_NODE_KIND_FILE)
        ++*static_cast<uint32_t*>(data);
    return index + 1;
}
Handle OpenChargedDisc(const std::filesystem::path& path,
                       NodDiscHeader& header, NodDiscMeta& metadata)
{
    NodHandle* raw_disc = nullptr;
    Check(nod_disc_open(PathUtf8(path).c_str(), nullptr, &raw_disc), "Cannot open disc");
    Handle disc(raw_disc, nod_free);
    Check(nod_disc_header(disc.get(), &header), "Cannot read disc header");
    Check(nod_disc_meta(disc.get(), &metadata), "Cannot read disc metadata");

    constexpr uint8_t wii_magic[] = {0x5D, 0x1C, 0x9E, 0xA3};
    const std::string game_id(header.game_id, sizeof(header.game_id));
    if (!std::equal(std::begin(wii_magic), std::end(wii_magic), header.wii_magic)
        || game_id.substr(0, 3) != "R4Q" || game_id.substr(4) != "01")
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

DiscInfo InspectDisc(const std::filesystem::path& path)
{
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    auto disc = OpenChargedDisc(path, header, metadata);
    auto partition = OpenGamePartition(disc.get());
    uint32_t file_count = 0;
    nod_partition_iterate_fst(partition.get(), CountFile, &file_count);
    if (const char* error = nod_error_message())
        throw std::runtime_error(std::string("Cannot read game file table: ") + error);
    if (file_count == 0)
        throw std::runtime_error("The game data partition has no files.");
    NodPartitionMeta partition_metadata{};
    Check(nod_partition_meta(partition.get(), &partition_metadata), "Cannot read the game's main.dol");
    const auto& dol = partition_metadata.raw_dol;
    if (!dol.data || dol.size == 0)
        throw std::runtime_error("The game data partition has no main.dol.");
    std::string dol_sha1 = Sha1Hex(static_cast<const unsigned char*>(dol.data), dol.size);

    return {metadata.format == NOD_FORMAT_RVZ ? "RVZ" : "ISO",
            std::string(header.game_id, sizeof(header.game_id)),
            std::string(header.game_title,
                        std::find(std::begin(header.game_title), std::end(header.game_title), '\0')),
            header.disc_version, file_count, std::move(dol_sha1)};
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
