#pragma once

#include "aes128.h"
#include "image.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mscharged::platform::wii_disc
{
constexpr std::uint32_t kHashesSize = 0x400;
constexpr std::uint32_t kSectorDataSize = kSectorSize - kHashesSize;
constexpr std::size_t kBootSize = 0x440;
constexpr std::size_t kPartitionHeaderSize = 0x2C0;

// A Wii partition as the partition table and its header describe it.
struct Partition
{
    std::uint32_t index = 0; // in disc order, over all partition groups
    std::uint32_t kind = 0;  // 0 data, 1 update, 2 channel, or the raw value
    std::uint32_t start_sector = 0;
    std::uint32_t data_start_sector = 0;
    std::uint32_t data_end_sector = 0;
    bool has_encryption = true;
    bool has_hashes = true;
    std::array<std::uint8_t, kPartitionHeaderSize> header{}; // ticket, TMD/cert/H3/data offsets
    Aes128Decryptor key;                                      // the decrypted title key

    explicit Partition(const std::uint8_t title_key[16]) : key(title_key) {}
    std::uint32_t Clusters() const { return data_end_sector - data_start_sector; }
    // Data bytes in one cluster: its 0x8000 bytes less the hash block.
    std::uint32_t ClusterDataSize() const { return has_hashes ? kSectorDataSize : kSectorSize; }
    // nod's data_size: the partition's sectors, hash blocks included.
    std::uint64_t DataSize() const { return std::uint64_t(Clusters()) * kSectorSize; }
};

// boot.bin, bi2.bin, apploader.img, fst.bin, main.dol and the partition's
// ticket, TMD, certificate chain and H3 table; empty when absent.
struct PartitionMeta
{
    std::vector<std::uint8_t> boot, bi2, apploader, fst, dol, ticket, tmd, cert_chain, h3_table;
};

// The FST of a partition (nod's Fst view). Names are the disc's bytes: nod
// decodes them from Shift JIS, which is the same for the ASCII names discs use.
class FstView
{
public:
    // False with nod's message when the root node or the string table is missing.
    bool Parse(const std::vector<std::uint8_t>& fst, std::string& error);
    std::uint32_t Count() const { return count_; }
    std::uint8_t KindByte(std::uint32_t node) const { return nodes_[std::size_t(node) * 12]; }
    bool IsFile(std::uint32_t node) const { return KindByte(node) == 0; }
    bool IsDirectory(std::uint32_t node) const { return KindByte(node) == 1; }
    // Files: the data offset >> 2; directories: the parent node.
    std::uint32_t Offset(std::uint32_t node) const;
    // Files: the size; directories: the node after their last child.
    std::uint32_t Length(std::uint32_t node) const;
    // The node's NUL-terminated name, or null when the string table lacks it.
    const char* Name(std::uint32_t node) const;
    // nod's find: ASCII-case-insensitive path segments; "" or "/" is the root
    // (0). Returns false when there is no such node.
    bool Find(const char* path, std::uint32_t& node) const;

private:
    const std::uint8_t* nodes_ = nullptr;
    std::uint32_t count_ = 0;
    const char* strings_ = nullptr;
    std::size_t strings_size_ = 0;
};

// An open Wii disc image. Reads are thread-safe; they share one cache of
// decrypted partition clusters.
class Disc
{
public:
    explicit Disc(const std::string& path);
    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;

    // The first 0x440 bytes: disc header, debug block, boot block.
    const std::uint8_t* Boot() const { return boot_.data(); }
    NodFormat Format() const { return image_.Format(); }
    NodDiscMeta Meta() const { return image_.Meta(); }
    // nod's disc_size: the ISO's size, or for a WBFS the DVD size it guesses.
    std::uint64_t Size() const { return size_; }
    const std::vector<Partition>& Partitions() const { return partitions_; }

    // Disc bytes as stored (nod's NOD_PARTITION_ENCRYPTION_ORIGINAL view).
    // Returns fewer bytes at the end of the disc; throws where a WBFS left out
    // a block of hashed partition data, which nod rebuilds and this build does not.
    std::size_t ReadRaw(std::uint64_t offset, std::uint8_t* out, std::size_t size);
    // Decrypted partition data; fewer bytes at the partition's or image's end.
    std::size_t ReadPartition(const Partition& partition, std::uint64_t offset, std::uint8_t* out, std::size_t size);
    // Copies the range if every cluster of it is cached: no I/O, allocation
    // or waiting for another reader; false otherwise, `out` untouched.
    bool TryReadCached(const Partition& partition, std::uint64_t offset, std::uint8_t* out, std::size_t size);
    PartitionMeta ReadMeta(const Partition& partition);

private:
    struct Slot
    {
        std::uint32_t partition = UINT32_MAX;
        std::uint32_t cluster = 0;
        std::uint64_t used = 0;
    };

    void ReadPartitions();
    // Disc bytes for metadata, before the disc size is known; throws when short.
    void ReadRawExact(std::uint64_t offset, std::uint8_t* out, std::size_t size, const std::string& context);
    void ReadPartitionExact(const Partition& partition, std::uint64_t offset, std::uint8_t* out, std::size_t size,
                            const std::string& context, bool bounded = true);
    // With `bounded` false, reads past the partition's recorded end (nod's
    // header and FST reads while opening the disc).
    std::size_t ReadPartitionLocked(const Partition& partition, std::uint64_t offset, std::uint8_t* out,
                                    std::size_t size, bool bounded);
    int FindSlot(std::uint32_t partition, std::uint32_t cluster) const;
    // The least recently used slot. Slots filled since are newer, so one load
    // never takes back a slot it filled.
    int TakeSlot();
    // Decrypts `cluster` and the next ones into the cache (a WIA/RVZ: copies
    // in the decoded group that holds it, and the next ones up to the same
    // read-ahead); -1 past the image.
    int LoadClusters(const Partition& partition, std::uint32_t cluster, bool bounded);
    std::uint8_t* SlotData(int slot) { return slot_data_.data() + std::size_t(slot) * kSectorSize; }

    Image image_;
    std::array<std::uint8_t, kBootSize> boot_{};
    std::vector<Partition> partitions_;
    std::uint64_t size_ = 0;

    std::mutex mutex_; // the image's file position and the cache
    std::vector<Slot> slots_;
    std::vector<std::uint8_t> slot_data_;
    std::vector<std::uint8_t> raw_;
    std::vector<SectorKind> kinds_;
    std::uint64_t clock_ = 0;
};
} // namespace mscharged::platform::wii_disc
