#pragma once

#include "image.h"
#include "lfg.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mscharged::platform::wii_disc
{
// Dolphin's WIA and RVZ images, after nod's io/wia.rs (MIT, Luke Street) and
// Dolphin's CC0 format description: data in compressed groups of one chunk,
// Wii partition data stored decrypted without hashes (0x7C00 bytes a sector),
// everything else as raw data. RVZ also packs junk as generator seeds. WIA
// purge compression is refused, as nod does, and chunks over 32 MiB (which
// nod reads). Reads are not thread-safe.
class RvzReader
{
public:
    RvzReader(const SplitFile& file, bool rvz);
    ~RvzReader();
    RvzReader(const RvzReader&) = delete;
    RvzReader& operator=(const RvzReader&) = delete;

    NodDiscMeta Meta() const;
    std::uint64_t DiscSize() const { return iso_size_; }
    std::uint32_t ChunkSize() const { return chunk_size_; }

    // Decrypted: partition data; Data or Zero: raw data; Missing: neither.
    SectorKind Kind(std::uint32_t sector) const;
    // A raw-data sector's 0x8000 bytes; the last group is kept decoded.
    void ReadRawSector(std::uint32_t sector, std::uint8_t* out);
    // The group of partition data that holds `sector`: its first sector, its
    // sector count and its data, valid until the next call.
    const std::uint8_t* ReadDecryptedGroup(std::uint32_t sector, std::uint32_t& first, std::uint32_t& count);

private:
    enum class Compression : std::uint32_t
    {
        None,
        Purge,
        Bzip2,
        Lzma,
        Lzma2,
        Zstandard,
    };

    struct Group
    {
        std::uint64_t offset;
        std::uint32_t size;        // stored bytes; 0: the data is zeroes
        bool compressed;           // RVZ may store a group as is
        std::uint32_t packed_size; // RVZ: the size before unpacking junk; 0: not packed
    };

    // A run of disc sectors and the groups that hold it.
    struct Area
    {
        bool partition;
        std::uint32_t first_sector;
        std::uint64_t end_offset; // raw data can end inside a sector
        std::uint32_t group_index;
        std::uint32_t partition_sector; // partition data: where the partition's data starts
    };

    struct GroupInfo
    {
        std::uint32_t index;
        std::uint32_t sector;
        std::uint32_t sectors;
        std::uint32_t size;           // decoded bytes
        std::uint64_t section_offset; // offset in the partition's data, or on the disc
        bool partition;
    };

    class Decompressor;

    const Area* Find(std::uint32_t sector) const;
    GroupInfo Locate(const Area& area, std::uint32_t sector) const;
    // A group's data, info.size bytes, without its exception lists.
    void DecodeGroup(const GroupInfo& info, std::uint8_t* out);
    // `count` entries of `entry` bytes, stored compressed in `stored` bytes.
    void ReadHeaders(std::uint64_t offset, std::uint32_t stored, std::size_t count, std::size_t entry,
                     std::vector<std::uint8_t>& out, const char* context);
    // RVZ packing: runs of data and of generated junk.
    void Unpack(const std::uint8_t* data, std::size_t size, std::uint8_t* out, const GroupInfo& info);

    const SplitFile& file_;
    bool rvz_;
    Compression compression_ = Compression::None;
    std::int32_t level_ = 0;
    std::uint32_t chunk_size_ = 0;
    std::uint64_t iso_size_ = 0;
    std::uint8_t disc_head_[0x80] = {};
    std::vector<Area> areas_; // partition data first, then raw data, as nod searches
    std::vector<Group> groups_;
    std::size_t max_decoded_ = 0; // the most bytes a group decompresses to

    // From an NKit header, if the image has one.
    std::uint32_t crc32_ = 0;
    std::uint8_t md5_[16] = {}, sha1_[20] = {};
    std::uint64_t xxh64_ = 0;

    std::unique_ptr<Decompressor> decompressor_;
    LaggedFibonacci lfg_;
    std::vector<std::uint8_t> stored_, decompressed_;
    std::vector<std::uint8_t> raw_group_, partition_group_;
    std::uint32_t raw_group_index_ = UINT32_MAX, partition_group_index_ = UINT32_MAX;
};
} // namespace mscharged::platform::wii_disc
