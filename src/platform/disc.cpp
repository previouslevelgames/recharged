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
}

DiscInfo InspectDisc(const std::filesystem::path& path)
{
    NodHandle* raw_disc = nullptr;
    Check(nod_disc_open(PathUtf8(path).c_str(), nullptr, &raw_disc), "Cannot open disc");
    Handle disc(raw_disc, nod_free);
    NodDiscHeader header{};
    NodDiscMeta metadata{};
    Check(nod_disc_header(disc.get(), &header), "Cannot read disc header");
    Check(nod_disc_meta(disc.get(), &metadata), "Cannot read disc metadata");

    constexpr uint8_t wii_magic[] = {0x5D, 0x1C, 0x9E, 0xA3};
    const std::string game_id(header.game_id, sizeof(header.game_id));
    if (!std::equal(std::begin(wii_magic), std::end(wii_magic), header.wii_magic)
        || game_id.substr(0, 3) != "R4Q" || game_id.substr(4) != "01")
        throw std::runtime_error("Expected a Mario Strikers Charged Wii image (R4Q?01).");
    if (metadata.format != NOD_FORMAT_ISO && metadata.format != NOD_FORMAT_RVZ)
        throw std::runtime_error("This build accepts ISO and RVZ disc images.");

    NodHandle* raw_partition = nullptr;
    Check(nod_disc_open_partition_kind(disc.get(), NOD_PARTITION_KIND_DATA, nullptr, &raw_partition),
          "Cannot open game data partition");
    Handle partition(raw_partition, nod_free);
    uint32_t file_count = 0;
    nod_partition_iterate_fst(partition.get(), CountFile, &file_count);
    if (const char* error = nod_error_message())
        throw std::runtime_error(std::string("Cannot read game file table: ") + error);
    if (file_count == 0)
        throw std::runtime_error("The game data partition has no files.");

    return {metadata.format == NOD_FORMAT_RVZ ? "RVZ" : "ISO", game_id,
            std::string(header.game_title,
                        std::find(std::begin(header.game_title), std::end(header.game_title), '\0')),
            header.disc_version, file_count};
}
}
