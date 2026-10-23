#include "disc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mscharged::platform::wii_disc
{
namespace
{
constexpr std::uint8_t kWiiMagic[4] = {0x5D, 0x1C, 0x9E, 0xA3};
constexpr std::uint8_t kGcnMagic[4] = {0xC2, 0x33, 0x9F, 0x3D};
constexpr std::uint64_t kPartitionTableOffset = 0x40000;
constexpr std::size_t kBi2Size = 0x2000;
constexpr std::size_t kApploaderOffset = kBootSize + kBi2Size;
constexpr std::size_t kTicketSize = 0x2A4;
constexpr std::size_t kH3TableSize = 0x18000;
constexpr std::size_t kDolHeaderSize = 0x100;
// 4 MiB of decrypted clusters, filled about half a MiB per miss: sequential
// file reads then mostly find their next bytes decrypted already.
constexpr std::size_t kCacheSlots = 128;
constexpr std::uint32_t kReadAhead = 16;
// Sizes the headers give for metadata (FST, TMD ...) are trusted up to this.
constexpr std::uint64_t kMaxMetadata = 64u << 20;
constexpr std::uint64_t kMiniDvdSize = 1459978240;
constexpr std::uint64_t kSingleLayerDvdSize = 4699979776;
constexpr std::uint64_t kDualLayerDvdSize = 8511160320;

// The Wii common keys, as nod's disc/wii.rs lists them: retail, Korean and
// vWii for retail tickets, then the same three for debug tickets.
constexpr std::uint8_t kRetailKeys[3][16] = {
    {0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4, 0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7},
    {0x63, 0xb8, 0x2b, 0xb4, 0xf4, 0x61, 0x4e, 0x2e, 0x13, 0xf2, 0xfe, 0xfb, 0xba, 0x4c, 0x9b, 0x7e},
    {0x30, 0xbf, 0xc7, 0x6e, 0x7c, 0x19, 0xaf, 0xbb, 0x23, 0x16, 0x33, 0x30, 0xce, 0xd7, 0xc2, 0x8d},
};
constexpr std::uint8_t kDebugKeys[3][16] = {
    {0xa1, 0x60, 0x4a, 0x6a, 0x71, 0x23, 0xb5, 0x29, 0xae, 0x8b, 0xec, 0x32, 0xc8, 0x16, 0xfc, 0xaa},
    {0x67, 0x45, 0x8b, 0x6b, 0xc6, 0x23, 0x7b, 0x32, 0x69, 0x98, 0x3c, 0x64, 0x73, 0x48, 0x33, 0x66},
    {0x2f, 0x5c, 0x1b, 0x29, 0x44, 0xe7, 0xfd, 0x6f, 0xc3, 0x97, 0x96, 0x4b, 0x05, 0x76, 0x91, 0xfa},
};

std::uint32_t Read32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

std::array<std::uint8_t, 16> DecryptTitleKey(const std::uint8_t* ticket)
{
    const auto* issuer = reinterpret_cast<const char*>(ticket + 0x140);
    const auto* end = static_cast<const char*>(std::memchr(issuer, 0, 64));
    if (!end || !IsValidUtf8(issuer, std::size_t(end - issuer))) ThrowFormat("failed to parse certificate issuer");
    const std::string name(issuer, end);
    const std::uint8_t(*keys)[16] = nullptr;
    if (name == "Root-CA00000001-XS00000003") keys = kRetailKeys;
    else if (name == "Root-CA00000002-XS00000006") keys = kDebugKeys;
    else ThrowFormat("unknown certificate issuer \"" + name + "\"");
    const unsigned index = ticket[0x1F1];
    if (index >= 3) ThrowFormat("unknown common key index " + std::to_string(index));
    // CBC with the title ID as the IV.
    std::uint8_t iv[16] = {};
    std::memcpy(iv, ticket + 0x1DC, 8);
    std::array<std::uint8_t, 16> key;
    Aes128Decryptor(keys[index]).DecryptCbc(iv, ticket + 0x1BF, key.data(), key.size());
    return key;
}

bool EqualIgnoringAsciiCase(const char* name, const char* segment, std::size_t length)
{
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; };
    for (std::size_t i = 0; i < length; ++i)
        if (!name[i] || lower(name[i]) != lower(segment[i])) return false;
    return name[length] == 0;
}
} // namespace

bool FstView::Parse(const std::vector<std::uint8_t>& fst, std::string& error)
{
    *this = FstView();
    if (fst.size() < 12)
    {
        error = "FST root node not found";
        return false;
    }
    // The root's length is the node count; the string table follows the nodes.
    const std::uint64_t strings = std::uint64_t(Read32(fst.data() + 8)) * 12;
    if (strings > fst.size())
    {
        error = "FST string table out of bounds";
        return false;
    }
    nodes_ = fst.data();
    count_ = std::uint32_t(strings / 12);
    strings_ = reinterpret_cast<const char*>(fst.data()) + strings;
    strings_size_ = fst.size() - std::size_t(strings);
    return true;
}

std::uint32_t FstView::Offset(std::uint32_t node) const { return Read32(nodes_ + std::size_t(node) * 12 + 4); }
std::uint32_t FstView::Length(std::uint32_t node) const { return Read32(nodes_ + std::size_t(node) * 12 + 8); }

const char* FstView::Name(std::uint32_t node) const
{
    const std::uint8_t* entry = nodes_ + std::size_t(node) * 12;
    const std::size_t offset = std::size_t(entry[1]) << 16 | std::size_t(entry[2]) << 8 | entry[3];
    if (offset >= strings_size_ || !std::memchr(strings_ + offset, 0, strings_size_ - offset)) return nullptr;
    return strings_ + offset;
}

bool FstView::Find(const char* path, std::uint32_t& node) const
{
    const char* cursor = path;
    const char* segment = nullptr;
    std::size_t length = 0;
    const auto next = [&] {
        while (*cursor == '/') ++cursor;
        segment = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        length = std::size_t(cursor - segment);
        return length != 0;
    };
    if (!next())
    {
        if (!count_) return false;
        node = 0;
        return true;
    }
    // Search the matched directory's children only. (nod's search runs on
    // past the directory's end, so a missing name can match a later node.)
    std::uint32_t index = 1, end = count_;
    while (index < end)
    {
        const char* name = Name(index);
        if (name && EqualIgnoringAsciiCase(name, segment, length))
        {
            if (!next())
            {
                node = index;
                return true;
            }
            if (!IsDirectory(index)) return false;
            end = std::min(Length(index), count_);
            ++index;
        }
        else if (IsDirectory(index))
            index = std::max(Length(index), index + 1);
        else
            ++index;
    }
    return false;
}

Disc::Disc(const std::string& path) : image_(path)
{
    slots_.resize(kCacheSlots);
    slot_data_.resize(kCacheSlots * kSectorSize);
    raw_.resize(std::size_t(kReadAhead) * kSectorSize);
    kinds_.resize(kReadAhead);
    std::lock_guard<std::mutex> lock(mutex_);
    ReadRawExact(0, boot_.data(), boot_.size(), "Reading disc headers");
    if (std::memcmp(boot_.data() + 0x18, kWiiMagic, 4) != 0)
    {
        if (std::memcmp(boot_.data() + 0x1C, kGcnMagic, 4) == 0)
            ThrowFormat("GameCube discs are not supported on this build; use a Wii disc image (ISO or WBFS)");
        ThrowFormat("Invalid disc header");
    }
    if (boot_[0x61] == 0 && boot_[0x60] != 0) ThrowFormat("Wii disc is encrypted but has no partition hashes");
    ReadPartitions();
    if (Format() == NOD_FORMAT_ISO)
    {
        size_ = IsoSize();
        return;
    }
    // nod's guess for formats that do not record the disc size.
    std::uint64_t end = partitions_.empty() ? 0x50000 : 0;
    bool data = false;
    for (const Partition& partition : partitions_)
    {
        end = std::max(end, std::uint64_t(partition.data_end_sector) * kSectorSize);
        data = data || partition.kind == 0;
    }
    size_ = end <= kMiniDvdSize && !data ? kMiniDvdSize : end < kSingleLayerDvdSize ? kSingleLayerDvdSize : kDualLayerDvdSize;
}

void Disc::ReadPartitions()
{
    const bool hashes = boot_[0x60] == 0, encryption = boot_[0x61] == 0;
    std::uint8_t groups[32];
    ReadRawExact(kPartitionTableOffset, groups, sizeof groups, "Reading partition groups");
    for (std::uint32_t group = 0; group < 4; ++group)
    {
        const std::uint32_t count = Read32(groups + group * 8);
        if (!count) continue;
        if (count > 0x1000)
            ThrowFormat("Partition group " + std::to_string(group) + " lists " + std::to_string(count) + " partitions");
        std::vector<std::uint8_t> entries(std::size_t(count) * 8);
        ReadRawExact(std::uint64_t(Read32(groups + group * 8 + 4)) << 2, entries.data(), entries.size(),
                     "Reading partition group " + std::to_string(group));
        for (std::uint32_t entry = 0; entry < count; ++entry)
        {
            const std::string name = std::to_string(group) + ":" + std::to_string(entry);
            const std::uint64_t offset = std::uint64_t(Read32(entries.data() + entry * 8)) << 2;
            std::array<std::uint8_t, kPartitionHeaderSize> header;
            ReadRawExact(offset, header.data(), header.size(), "Reading partition header " + name);
            const auto title_key = DecryptTitleKey(header.data());
            if (offset % kSectorSize) ThrowFormat("Partition " + name + " offset is not sector aligned");
            const std::uint64_t data_start = offset + (std::uint64_t(Read32(header.data() + 0x2B8)) << 2);
            const std::uint64_t data_size = std::uint64_t(Read32(header.data() + 0x2BC)) << 2;
            if (data_start % kSectorSize || (data_start + data_size) % kSectorSize)
                ThrowFormat("Partition " + name + " data is not sector aligned");
            Partition partition(title_key.data());
            partition.index = std::uint32_t(partitions_.size());
            partition.kind = Read32(entries.data() + entry * 8 + 4);
            partition.start_sector = std::uint32_t(offset / kSectorSize);
            partition.data_start_sector = std::uint32_t(data_start / kSectorSize);
            partition.data_end_sector = std::uint32_t((data_start + data_size) / kSectorSize);
            partition.has_encryption = encryption;
            partition.has_hashes = hashes;
            partition.header = header;
            // nod checks each partition's boot block and FST while opening.
            std::uint8_t boot[kBootSize];
            ReadPartitionExact(partition, 0, boot, sizeof boot, "Reading boot data", false);
            if (std::memcmp(boot + 0x18, kWiiMagic, 4) == 0)
            {
                const std::uint64_t fst_offset = std::uint64_t(Read32(boot + 0x424)) << 2;
                const std::uint64_t fst_size = std::uint64_t(Read32(boot + 0x428)) << 2;
                const std::string context = "Reading partition FST (offset " + std::to_string(fst_offset) + ", size "
                    + std::to_string(fst_size) + ")";
                if (fst_size > kMaxMetadata) ThrowIo(context, "the FST is larger than this build reads");
                std::vector<std::uint8_t> fst(static_cast<std::size_t>(fst_size));
                ReadPartitionExact(partition, fst_offset, fst.data(), fst.size(), context, false);
                FstView view;
                std::string invalid;
                if (view.Parse(fst, invalid))
                {
                    std::uint64_t end = 0;
                    for (std::uint32_t node = 0; node < view.Count(); ++node)
                        if (view.IsFile(node)) end = std::max(end, (std::uint64_t(view.Offset(node)) << 2) + view.Length(node));
                    if (end > data_size)
                    {
                        if (data_size) ThrowFormat("Partition " + name + " FST exceeds data size");
                        // A decrypted image without a data size: as nod, take
                        // it from the FST (counted from the data's start).
                        const std::uint32_t per_cluster = partition.ClusterDataSize();
                        partition.data_end_sector =
                            partition.data_start_sector + std::uint32_t((end + per_cluster - 1) / per_cluster);
                    }
                }
            }
            partitions_.push_back(partition);
        }
    }
}

void Disc::ReadRawExact(std::uint64_t offset, std::uint8_t* out, std::size_t size, const std::string& context)
{
    while (size)
    {
        if (offset / kSectorSize > UINT32_MAX) ThrowIo(context, "failed to fill whole buffer");
        const auto sector = std::uint32_t(offset / kSectorSize);
        std::size_t within = std::size_t(offset % kSectorSize);
        const auto count = std::uint32_t(std::min<std::uint64_t>(kReadAhead, (within + size + kSectorSize - 1) / kSectorSize));
        try
        {
            image_.ReadSectors(sector, count, raw_.data(), kinds_.data());
        }
        catch (const Error& error)
        {
            ThrowIo(context, error.what());
        }
        for (std::uint32_t i = 0; i < count && size; ++i)
        {
            if (kinds_[i] == SectorKind::Missing) ThrowIo(context, "failed to fill whole buffer");
            const std::size_t part = std::min<std::size_t>(kSectorSize - within, size);
            std::memcpy(out, raw_.data() + std::size_t(i) * kSectorSize + within, part);
            out += part;
            offset += part;
            size -= part;
            within = 0;
        }
    }
}

void Disc::ReadPartitionExact(const Partition& partition, std::uint64_t offset, std::uint8_t* out, std::size_t size,
                              const std::string& context, bool bounded)
{
    std::size_t done = 0;
    try
    {
        done = ReadPartitionLocked(partition, offset, out, size, bounded);
    }
    catch (const Error& error)
    {
        ThrowIo(context, error.what());
    }
    if (done != size) ThrowIo(context, "failed to fill whole buffer");
}

int Disc::FindSlot(std::uint32_t partition, std::uint32_t cluster) const
{
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].partition == partition && slots_[i].cluster == cluster) return int(i);
    return -1;
}

int Disc::LoadClusters(const Partition& partition, std::uint32_t cluster, bool bounded)
{
    std::uint64_t limit = std::uint64_t(UINT32_MAX) - partition.data_start_sector + 1;
    if (bounded) limit = std::min<std::uint64_t>(limit, partition.Clusters());
    std::uint32_t count = std::uint32_t(std::min<std::uint64_t>(kReadAhead, limit - cluster));
    // Stop before clusters that are decrypted already.
    for (std::uint32_t i = 1; i < count; ++i)
        if (FindSlot(partition.index, cluster + i) >= 0)
        {
            count = i;
            break;
        }
    image_.ReadSectors(partition.data_start_sector + cluster, count, raw_.data(), kinds_.data());
    int first = -1;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        // Reads end where the image does (nod: an empty block ends the stream).
        if (kinds_[i] == SectorKind::Missing) break;
        // The least recently used slot; this batch's are the newest, so it
        // never takes one of them.
        std::size_t slot = 0;
        for (std::size_t s = 0; s < slots_.size(); ++s)
        {
            if (slots_[s].partition == UINT32_MAX)
            {
                slot = s;
                break;
            }
            if (slots_[s].used < slots_[slot].used) slot = s;
        }
        slots_[slot] = {partition.index, cluster + i, ++clock_};
        std::uint8_t* data = SlotData(int(slot));
        const std::uint8_t* sector = raw_.data() + std::size_t(i) * kSectorSize;
        if (kinds_[i] == SectorKind::Zero)
            std::memset(data, 0, kSectorSize); // a WBFS leaves out unused blocks; nod reads zeroes
        else if (partition.has_encryption)
            // The data's IV is the end of the still encrypted hash block.
            partition.key.DecryptCbc(sector + 0x3D0, sector + kHashesSize, data, kSectorDataSize);
        else if (partition.has_hashes)
            std::memcpy(data, sector + kHashesSize, kSectorDataSize);
        else
            std::memcpy(data, sector, kSectorSize);
        if (i == 0) first = int(slot);
    }
    return first;
}

std::size_t Disc::ReadPartitionLocked(const Partition& partition, std::uint64_t offset, std::uint8_t* out,
                                      std::size_t size, bool bounded)
{
    const std::uint32_t per_cluster = partition.ClusterDataSize();
    std::size_t done = 0;
    while (done < size)
    {
        const std::uint64_t position = offset + done;
        if (position < offset) break;
        const std::uint64_t cluster = position / per_cluster;
        if (bounded ? cluster >= partition.Clusters() : cluster > UINT32_MAX - partition.data_start_sector) break;
        int slot = FindSlot(partition.index, std::uint32_t(cluster));
        if (slot >= 0) slots_[std::size_t(slot)].used = ++clock_;
        else slot = LoadClusters(partition, std::uint32_t(cluster), bounded);
        if (slot < 0) break;
        const std::size_t within = std::size_t(position % per_cluster);
        const std::size_t count = std::min<std::size_t>(per_cluster - within, size - done);
        std::memcpy(out + done, SlotData(slot) + within, count);
        done += count;
    }
    return done;
}

std::size_t Disc::ReadPartition(const Partition& partition, std::uint64_t offset, std::uint8_t* out, std::size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadPartitionLocked(partition, offset, out, size, true);
}

bool Disc::TryReadCached(const Partition& partition, std::uint64_t offset, std::uint8_t* out, std::size_t size)
{
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !size) return false;
    const std::uint32_t per_cluster = partition.ClusterDataSize();
    const std::uint64_t end = offset + size;
    if (end < offset) return false;
    const std::uint64_t first = offset / per_cluster, last = (end - 1) / per_cluster;
    int slots[16];
    if (last >= partition.Clusters() || last - first >= 16) return false;
    // Every cluster before any byte: a miss leaves `out` as it was.
    for (std::uint64_t cluster = first; cluster <= last; ++cluster)
        if ((slots[cluster - first] = FindSlot(partition.index, std::uint32_t(cluster))) < 0) return false;
    std::size_t done = 0;
    for (std::uint64_t cluster = first; cluster <= last; ++cluster)
    {
        const std::size_t within = std::size_t((offset + done) % per_cluster);
        const std::size_t count = std::min<std::size_t>(per_cluster - within, size - done);
        std::memcpy(out + done, SlotData(slots[cluster - first]) + within, count);
        done += count;
    }
    return true;
}

std::size_t Disc::ReadRaw(std::uint64_t offset, std::uint8_t* out, std::size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset >= size_) return 0;
    size = std::size_t(std::min<std::uint64_t>(size, size_ - offset));
    std::size_t done = 0;
    while (done < size)
    {
        const std::uint64_t position = offset + done;
        const auto sector = std::uint32_t(position / kSectorSize);
        std::size_t within = std::size_t(position % kSectorSize);
        const auto count =
            std::uint32_t(std::min<std::uint64_t>(kReadAhead, (within + (size - done) + kSectorSize - 1) / kSectorSize));
        image_.ReadSectors(sector, count, raw_.data(), kinds_.data());
        for (std::uint32_t i = 0; i < count && done < size; ++i)
        {
            if (kinds_[i] == SectorKind::Missing) return done;
            if (kinds_[i] == SectorKind::Zero)
            {
                // nod rebuilds the hashes and encryption of partition data a
                // WBFS leaves out; zeroes would be wrong, so fail instead.
                const std::uint32_t at = sector + i;
                const bool hashed = std::any_of(partitions_.begin(), partitions_.end(), [at](const Partition& p) {
                    return p.has_hashes && at >= p.data_start_sector && at < p.data_end_sector;
                });
                if (hashed)
                {
                    if (done) return done;
                    throw Error(NOD_RESULT_ERR_IO, "Reading disc sector " + std::to_string(at)
                                                       + ": the image leaves out this partition data, and this build "
                                                         "cannot rebuild its hashes and encryption");
                }
            }
            const std::size_t part = std::min<std::size_t>(kSectorSize - within, size - done);
            std::memcpy(out + done, raw_.data() + std::size_t(i) * kSectorSize + within, part);
            done += part;
            within = 0;
        }
    }
    return done;
}

PartitionMeta Disc::ReadMeta(const Partition& partition)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PartitionMeta meta;
    const std::uint64_t capacity = std::uint64_t(partition.Clusters()) * partition.ClusterDataSize();
    const auto read = [&](std::vector<std::uint8_t>& out, std::uint64_t offset, std::uint64_t size,
                          const std::string& context) {
        // More than the partition holds cannot be read in full.
        if (size > capacity || size > kMaxMetadata) ThrowIo(context, "failed to fill whole buffer");
        out.resize(std::size_t(size));
        ReadPartitionExact(partition, offset, out.data(), out.size(), context);
    };
    read(meta.boot, 0, kBootSize, "Reading boot.bin");
    read(meta.bi2, kBootSize, kBi2Size, "Reading bi2.bin");
    std::uint8_t apploader[0x20];
    ReadPartitionExact(partition, kApploaderOffset, apploader, sizeof apploader, "Reading apploader header");
    read(meta.apploader, kApploaderOffset,
         sizeof apploader + std::uint64_t(Read32(apploader + 0x14)) + Read32(apploader + 0x18), "Reading apploader");
    const std::uint8_t* boot = meta.boot.data();
    const std::uint64_t fst_offset = std::uint64_t(Read32(boot + 0x424)) << 2;
    const std::uint64_t fst_size = std::uint64_t(Read32(boot + 0x428)) << 2;
    read(meta.fst, fst_offset, fst_size,
         "Reading partition FST (offset " + std::to_string(fst_offset) + ", size " + std::to_string(fst_size) + ")");
    // main.dol: its header, then up to the end of its last section.
    const std::uint64_t dol_offset = std::uint64_t(Read32(boot + 0x420)) << 2;
    std::uint8_t dol[kDolHeaderSize];
    ReadPartitionExact(partition, dol_offset, dol, sizeof dol, "Reading DOL header");
    std::uint64_t dol_size = kDolHeaderSize;
    for (int section = 0; section < 18; ++section)
    {
        // Seven text then eleven data sections: offsets at 0x00, sizes at 0x90.
        const std::uint64_t end = std::uint64_t(Read32(dol + section * 4)) + Read32(dol + 0x90 + section * 4);
        dol_size = std::max(dol_size, end);
    }
    read(meta.dol, dol_offset, dol_size, "Reading DOL");
    // Ticket, TMD, certificate chain and H3 table come from the partition
    // header and its unencrypted area.
    const std::uint8_t* header = partition.header.data();
    meta.ticket.assign(header, header + kTicketSize);
    const std::uint64_t start = std::uint64_t(partition.start_sector) * kSectorSize;
    const auto read_raw = [&](std::vector<std::uint8_t>& out, std::uint64_t offset, std::uint64_t size,
                              const std::string& context) {
        if (size > kMaxMetadata) ThrowIo(context, "the size is larger than this build reads");
        out.resize(std::size_t(size));
        ReadRawExact(start + offset, out.data(), out.size(), context);
    };
    if (const std::uint32_t size = Read32(header + 0x2A4))
        read_raw(meta.tmd, std::uint64_t(Read32(header + 0x2A8)) << 2, size, "Reading TMD");
    if (const std::uint32_t size = Read32(header + 0x2AC))
        read_raw(meta.cert_chain, std::uint64_t(Read32(header + 0x2B0)) << 2, size, "Reading cert chain");
    if (partition.has_hashes)
        read_raw(meta.h3_table, std::uint64_t(Read32(header + 0x2B4)) << 2, kH3TableSize, "Reading H3 table");
    return meta;
}
} // namespace mscharged::platform::wii_disc
