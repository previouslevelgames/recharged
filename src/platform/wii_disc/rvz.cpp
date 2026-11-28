#include "rvz.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>

namespace mscharged::platform::wii_disc
{
namespace
{
constexpr std::size_t kFileHeaderSize = 0x48;
constexpr std::size_t kDiscStructSize = 0xDC;
constexpr std::size_t kPartitionStructSize = 0x30;
constexpr std::uint32_t kCompressedBit = 0x80000000u;
// Each group of partition data starts with one exception list per 2 MiB of
// its chunk; Dolphin reads at most 3328 exceptions a list.
constexpr std::size_t kExceptionSize = 22, kMaxExceptions = 3328;
// Header sizes the image gives are trusted up to this.
constexpr std::uint64_t kMaxHeaders = 64u << 20;
// Dolphin writes chunks of 32 KiB to 2 MiB (WIA: 2 MiB); the cache and the
// decoding buffers grow with the chunk, so far bigger ones are refused.
constexpr std::uint32_t kMaxChunkSize = 32u << 20;

std::uint16_t Read16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }

std::uint32_t Read32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

std::uint64_t Read64(const std::uint8_t* p) { return std::uint64_t(Read32(p)) << 32 | Read32(p + 4); }

std::string Hex(std::uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof text, "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

// SHA-1 (FIPS 180-4), for the hashes WIA and RVZ keep of their own headers.
std::array<std::uint8_t, 20> Sha1(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const auto rotate = [](std::uint32_t value, int bits) { return value << bits | value >> (32 - bits); };
    const auto block = [&](const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = Read32(p + 4 * i);
        for (int i = 16; i < 80; ++i) w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            const std::uint32_t f = i < 20 ? (b & c) | (~b & d)
                : i < 40                   ? b ^ c ^ d
                : i < 60                   ? (b & c) | (b & d) | (c & d)
                                           : b ^ c ^ d;
            const std::uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            const std::uint32_t t = rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotate(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    };
    std::size_t done = 0;
    for (; size - done >= 64; done += 64) block(data + done);
    std::uint8_t tail[128] = {};
    std::memcpy(tail, data + done, size - done);
    tail[size - done] = 0x80;
    const std::size_t padded = size - done < 56 ? 64 : 128;
    const std::uint64_t bits = std::uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[padded - 1 - i] = std::uint8_t(bits >> (8 * i));
    block(tail);
    if (padded == 128) block(tail + 64);
    std::array<std::uint8_t, 20> digest;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) digest[4 * i + j] = std::uint8_t(h[i] >> (24 - 8 * j));
    return digest;
}

std::string HexBytes(const std::uint8_t* bytes, std::size_t size)
{
    std::string text;
    char digits[3];
    for (std::size_t i = 0; i < size; ++i)
    {
        std::snprintf(digits, sizeof digits, "%02x", bytes[i]);
        text += digits;
    }
    return text;
}

void VerifyHash(const std::uint8_t* data, std::size_t size, const std::uint8_t* expected)
{
    const auto digest = Sha1(data, size);
    if (std::memcmp(digest.data(), expected, digest.size()) != 0)
        ThrowFormat("WIA/RVZ hash mismatch: " + HexBytes(digest.data(), digest.size()) + ", expected "
                    + HexBytes(expected, digest.size()));
}

// Skips a group's exception lists (only hash recovery uses them); stored ones
// are padded to four bytes. Returns their size.
std::size_t SkipExceptionLists(const std::uint8_t* data, std::size_t size, std::uint32_t chunk_size, bool padded)
{
    std::size_t position = 0;
    for (std::uint32_t list = 0; list < (chunk_size + 0x1FFFFF) / 0x200000; ++list)
    {
        if (size - position < 2) throw Error(NOD_RESULT_ERR_IO, "Reading WIA/RVZ exception list count");
        const std::size_t count = Read16(data + position);
        position += 2;
        if (size - position < count * kExceptionSize) throw Error(NOD_RESULT_ERR_IO, "Reading WIA/RVZ exception list");
        position += count * kExceptionSize;
    }
    if (padded && position % 4)
    {
        position += 4 - position % 4;
        if (position > size) throw Error(NOD_RESULT_ERR_IO, "Reading WIA/RVZ exception list padding");
    }
    return position;
}
} // namespace

// One codec for the whole image: zstd's context and liblzma's decoder (with
// its dictionary) are made once and reused group after group.
class RvzReader::Decompressor
{
public:
    Decompressor(Compression kind, const std::uint8_t* props, std::size_t props_size) : kind_(kind)
    {
        if (kind == Compression::Zstandard)
        {
            zstd_ = ZSTD_createDCtx();
            if (!zstd_) throw std::bad_alloc();
        }
        if (kind != Compression::Lzma && kind != Compression::Lzma2) return;
        // 7-Zip's property bytes, as WIA and RVZ store them.
        if (lzma_lzma_preset(&options_, LZMA_PRESET_DEFAULT)) ThrowFormat("LZMA presets are unavailable");
        if (kind == Compression::Lzma)
        {
            if (props_size != 5) ThrowFormat("Invalid LZMA props length: " + std::to_string(props_size));
            unsigned value = props[0];
            if (value >= 9 * 5 * 5) ThrowFormat("Invalid LZMA props byte: " + std::to_string(value));
            options_.lc = value % 9;
            value /= 9;
            options_.pb = value / 5;
            options_.lp = value % 5;
            dictionary_ = std::uint32_t(props[1]) | std::uint32_t(props[2]) << 8 | std::uint32_t(props[3]) << 16
                | std::uint32_t(props[4]) << 24;
        }
        else
        {
            if (props_size != 1) ThrowFormat("Invalid LZMA2 props length: " + std::to_string(props_size));
            const unsigned value = props[0];
            if (value > 40) ThrowFormat("Invalid LZMA2 props byte: " + std::to_string(value));
            dictionary_ = value == 40 ? UINT32_MAX : (2u | (value & 1)) << (value / 2 + 11);
        }
    }

    ~Decompressor()
    {
        if (zstd_) ZSTD_freeDCtx(zstd_);
        lzma_end(&lzma_);
    }

    // The size a stream decompresses to, if it says so; 0 if it does not.
    std::size_t ContentSize(const std::uint8_t* in, std::size_t size) const
    {
        if (kind_ == Compression::None || kind_ == Compression::Purge) return size;
        if (kind_ != Compression::Zstandard) return 0;
        const unsigned long long content = ZSTD_getFrameContentSize(in, size);
        if (content == ZSTD_CONTENTSIZE_ERROR) throw Error(NOD_RESULT_ERR_IO, "Invalid Zstandard frame header");
        if (content == ZSTD_CONTENTSIZE_UNKNOWN || content > SIZE_MAX) return 0;
        return std::size_t(content);
    }

    // Decompresses one whole stream into at most `capacity` bytes. Streams never
    // reach further back than their own start, so a dictionary as large as the
    // largest output decodes exactly what the stated one would.
    std::size_t Decompress(const std::uint8_t* in, std::size_t size, std::uint8_t* out, std::size_t capacity,
                           std::size_t largest)
    {
        switch (kind_)
        {
        case Compression::None:
        case Compression::Purge:
            if (size > capacity)
                throw Error(NOD_RESULT_ERR_IO, "Decompressed data too large: " + std::to_string(size) + " > "
                                                   + std::to_string(capacity));
            std::memcpy(out, in, size);
            return size;
        case Compression::Zstandard:
        {
            const std::size_t result = ZSTD_decompressDCtx(zstd_, out, capacity, in, size);
            if (ZSTD_isError(result)) throw Error(NOD_RESULT_ERR_IO, ZSTD_getErrorName(result));
            return result;
        }
        case Compression::Bzip2: return Bzip2(in, size, out, capacity);
        case Compression::Lzma:
        case Compression::Lzma2: return Lzma(in, size, out, capacity, largest);
        }
        return 0;
    }

private:
    static std::size_t Bzip2(const std::uint8_t* in, std::size_t size, std::uint8_t* out, std::size_t capacity)
    {
        if (size > UINT_MAX || capacity > UINT_MAX) throw Error(NOD_RESULT_ERR_IO, "bzip2 buffer exceeds its limits");
        bz_stream stream{};
        stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(in));
        stream.avail_in = unsigned(size);
        stream.next_out = reinterpret_cast<char*>(out);
        stream.avail_out = unsigned(capacity);
        int result = BZ2_bzDecompressInit(&stream, 0, 0);
        if (result != BZ_OK)
            throw Error(NOD_RESULT_ERR_IO, "bzip2 decompressor init failed with code " + std::to_string(result));
        // As nod: decompress until the stream ends, the output fills or the input runs out.
        result = BZ_OK;
        while (stream.avail_out > 0)
        {
            result = BZ2_bzDecompress(&stream);
            if (result != BZ_OK || stream.avail_in == 0) break;
        }
        const std::size_t written = capacity - stream.avail_out;
        BZ2_bzDecompressEnd(&stream);
        if (result == BZ_STREAM_END) return written;
        if (result == BZ_OK && written == capacity)
            throw Error(NOD_RESULT_ERR_IO, "bzip2 decompression output buffer too small");
        throw Error(NOD_RESULT_ERR_IO, "bzip2 decompression failed with code " + std::to_string(result));
    }

    std::size_t Lzma(const std::uint8_t* in, std::size_t size, std::uint8_t* out, std::size_t capacity,
                     std::size_t largest)
    {
        options_.dict_size =
            std::uint32_t(std::max<std::uint64_t>(LZMA_DICT_SIZE_MIN, std::min<std::uint64_t>(dictionary_, largest)));
        const lzma_filter filters[2] = {{kind_ == Compression::Lzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, &options_},
                                        {LZMA_VLI_UNKNOWN, nullptr}};
        // Re-initialising keeps the previous decoder's memory when it fits.
        lzma_ret result = lzma_raw_decoder(&lzma_, filters);
        if (result != LZMA_OK)
            throw Error(NOD_RESULT_ERR_IO, "LZMA decoder init failed with code " + std::to_string(int(result)));
        lzma_.next_in = in;
        lzma_.avail_in = size;
        lzma_.next_out = out;
        lzma_.avail_out = capacity;
        result = lzma_code(&lzma_, LZMA_FINISH);
        if (result != LZMA_STREAM_END)
        {
            const char* reason = result == LZMA_OK && lzma_.avail_out == 0 ? "output buffer too small"
                : result == LZMA_OK || result == LZMA_DATA_ERROR || result == LZMA_BUF_ERROR
                ? "data error"
                : "unknown error";
            throw Error(NOD_RESULT_ERR_IO, std::string("LZMA decompression failed: ") + reason + " ("
                                               + std::to_string(int(result)) + ")");
        }
        if (lzma_.avail_in)
            throw Error(NOD_RESULT_ERR_IO, "LZMA decompression consumed " + std::to_string(size - lzma_.avail_in)
                                               + " of " + std::to_string(size) + " bytes");
        return capacity - lzma_.avail_out;
    }

    Compression kind_;
    ZSTD_DCtx* zstd_ = nullptr;
    lzma_stream lzma_ = LZMA_STREAM_INIT;
    lzma_options_lzma options_{};
    std::uint32_t dictionary_ = 0;
};

RvzReader::RvzReader(const SplitFile& file, bool rvz) : file_(file), rvz_(rvz)
{
    std::uint8_t header[kFileHeaderSize];
    if (file.Size() < sizeof header) ThrowIo("Reading WIA/RVZ file header", "failed to fill whole buffer");
    file.ReadAt(0, header, sizeof header);
    const std::uint32_t version = Read32(header + 4), compatible = Read32(header + 8), disc_size = Read32(header + 12);
    if (0x01000000u < compatible || (rvz ? 0x00030000u : 0x00080000u) > version)
        ThrowFormat("Unsupported WIA/RVZ version: " + Hex(version));
    VerifyHash(header, 0x34, header + 0x34);
    if (compatible < 0x30000) ThrowFormat("WIA/RVZ version " + Hex(compatible) + " is not supported");
    iso_size_ = Read64(header + 0x24);

    // The disc struct, zero-filled when an older one is shorter.
    if (disc_size > 0x10000) ThrowFormat("WIA/RVZ disc header size " + std::to_string(disc_size) + " is invalid");
    std::vector<std::uint8_t> disc(disc_size);
    if (file.Size() < kFileHeaderSize + disc_size) ThrowIo("Reading WIA/RVZ disc header", "failed to fill whole buffer");
    file.ReadAt(kFileHeaderSize, disc.data(), disc.size());
    VerifyHash(disc.data(), disc.size(), header + 16);
    disc.resize(std::max(disc.size(), kDiscStructSize), 0);
    const std::uint32_t disc_type = Read32(&disc[0]), compression = Read32(&disc[4]);
    level_ = std::int32_t(Read32(&disc[8]));
    chunk_size_ = Read32(&disc[12]);
    if (disc_type != 1 && disc_type != 2) ThrowFormat("Invalid disc type " + std::to_string(disc_type));
    if (compression > 5) ThrowFormat("Invalid compression type " + std::to_string(compression));
    compression_ = Compression(compression);
    if (rvz && (chunk_size_ < kSectorSize || (chunk_size_ & (chunk_size_ - 1))))
        ThrowFormat("Invalid RVZ chunk size: " + Hex(chunk_size_));
    if (!rvz && (chunk_size_ < 0x200000 || chunk_size_ % 0x200000))
        ThrowFormat("Invalid WIA chunk size: " + Hex(chunk_size_));
    if (chunk_size_ > kMaxChunkSize)
        ThrowFormat("WIA/RVZ chunk size " + Hex(chunk_size_) + " is larger than this build reads (32 MiB)");
    if (Read32(&disc[0x94]) != kPartitionStructSize)
        ThrowFormat("WIA/RVZ partition type size is " + std::to_string(Read32(&disc[0x94])) + ", expected 48");
    if (compression_ == Compression::Purge)
        ThrowFormat("WIA images compressed with purge are not supported on this build; use ISO, WBFS or RVZ");
    std::memcpy(disc_head_, &disc[16], sizeof disc_head_);

    // nod reads an NKit header's disc hashes from just after the disc struct.
    const std::uint64_t nkit = kFileHeaderSize + disc_size;
    std::uint8_t nkit_bytes[0x200] = {};
    if (file.Size() >= nkit + 64)
    {
        const std::size_t available = std::size_t(std::min<std::uint64_t>(sizeof nkit_bytes, file.Size() - nkit));
        file.ReadAt(nkit, nkit_bytes, available);
        if (std::memcmp(nkit_bytes, "NKIT  v", 7) == 0 && (nkit_bytes[7] == '1' || nkit_bytes[7] == '2'))
        {
            // Version 1 has CRC32, MD5, SHA-1 and XXH64; version 2 says what it has.
            const bool v2 = nkit_bytes[7] == '2';
            const std::size_t size = v2 ? Read16(nkit_bytes + 8) : 56;
            const std::uint16_t flags = v2 ? Read16(nkit_bytes + 10) : 0x1E;
            std::size_t at = v2 ? 12 : 8;
            const auto take = [&](std::size_t bytes) {
                const std::size_t start = at;
                at += bytes;
                return at <= size && at <= available ? nkit_bytes + start : nullptr;
            };
            const std::uint8_t* fields[5] = {};
            for (int bit = 0; bit < 5; ++bit)
                if (flags & (1 << bit)) fields[bit] = take(bit == 0 ? 8 : bit == 1 ? 4 : bit == 2 ? 16 : bit == 3 ? 20 : 8);
            if (at <= size && at <= available)
            {
                if (fields[1]) crc32_ = Read32(fields[1]);
                if (fields[2]) std::memcpy(md5_, fields[2], sizeof md5_);
                if (fields[3]) std::memcpy(sha1_, fields[3], sizeof sha1_);
                if (fields[4]) xxh64_ = Read64(fields[4]);
            }
        }
    }

    // The partitions' data, then the raw data around it (nod's search order).
    const std::uint32_t partitions = Read32(&disc[0x90]);
    if (partitions > 0x1000) ThrowFormat("WIA/RVZ image lists " + std::to_string(partitions) + " partitions");
    std::vector<std::uint8_t> partition_structs(partitions * kPartitionStructSize);
    try
    {
        file.ReadAt(Read64(&disc[0x98]), partition_structs.data(), partition_structs.size());
    }
    catch (const Error& error)
    {
        ThrowIo("Reading WIA/RVZ partition headers", error.what());
    }
    VerifyHash(partition_structs.data(), partition_structs.size(), &disc[0xA0]);
    for (std::uint32_t partition = 0; partition < partitions; ++partition)
    {
        const std::uint8_t* entry = &partition_structs[partition * kPartitionStructSize];
        const std::uint32_t first = Read32(entry + 16);
        for (int part = 0; part < 2; ++part)
        {
            const std::uint8_t* data = entry + 16 + 16 * part;
            if (Read32(data + 4) == 0) continue;
            areas_.push_back({true, Read32(data), (std::uint64_t(Read32(data)) + Read32(data + 4)) * kSectorSize,
                              Read32(data + 8), first});
        }
    }
    // nod slices the 7 bytes there are by this length (and panics past them).
    if (disc[0xD4] > 7) ThrowFormat("Invalid compressor data length " + std::to_string(disc[0xD4]));
    decompressor_ = std::make_unique<Decompressor>(compression_, &disc[0xD5], disc[0xD4]);

    std::vector<std::uint8_t> raw;
    ReadHeaders(Read64(&disc[0xB8]), Read32(&disc[0xC0]), Read32(&disc[0xB4]), 0x18, raw, "WIA/RVZ raw data headers");
    const std::size_t raw_count = raw.size() / 0x18;
    for (std::size_t i = 0; i < raw_count; ++i)
    {
        const std::uint8_t* entry = &raw[i * 0x18];
        // The first starts at 0x80, after the disc struct's copy: its sector begins at 0.
        const std::uint64_t offset = Read64(entry), end = offset + Read64(entry + 8);
        const std::uint64_t start = offset & ~std::uint64_t(kSectorSize - 1);
        if (i + 1 < raw_count && end % kSectorSize)
            ThrowFormat("WIA/RVZ raw data " + std::to_string(i) + " not aligned to sector: " + Hex(start) + ".."
                        + Hex(end));
        if (start / kSectorSize > UINT32_MAX) ThrowFormat("WIA/RVZ raw data " + std::to_string(i) + " is past any disc");
        areas_.push_back({false, std::uint32_t(start / kSectorSize), end, Read32(entry + 16), 0});
    }

    std::vector<std::uint8_t> groups;
    const std::size_t entry_size = rvz ? 12 : 8;
    ReadHeaders(Read64(&disc[0xC8]), Read32(&disc[0xD0]), Read32(&disc[0xC4]), entry_size, groups,
                "WIA/RVZ group headers");
    groups_.resize(groups.size() / entry_size);
    // Room for a group's data and its exception lists, as decompressed.
    const std::size_t lists = (chunk_size_ + 0x1FFFFF) / 0x200000;
    max_decoded_ = chunk_size_ + (2 + kMaxExceptions * kExceptionSize) * lists;
    for (std::size_t i = 0; i < groups_.size(); ++i)
    {
        const std::uint8_t* entry = &groups[i * entry_size];
        const std::uint32_t size = Read32(entry + 4);
        // WIA groups are always compressed (with WIA's "none", stored as is).
        groups_[i] = rvz ? Group{std::uint64_t(Read32(entry)) * 4, size & ~kCompressedBit, (size & kCompressedBit) != 0,
                                 Read32(entry + 8)}
                         : Group{std::uint64_t(Read32(entry)) * 4, size, true, 0};
        max_decoded_ = std::max<std::size_t>(max_decoded_, groups_[i].packed_size + (2 + kMaxExceptions * kExceptionSize) * lists);
    }
}

RvzReader::~RvzReader() = default;

void RvzReader::ReadHeaders(std::uint64_t offset, std::uint32_t stored, std::size_t count, std::size_t entry,
                            std::vector<std::uint8_t>& out, const char* context)
{
    if (count * entry > kMaxHeaders || stored > kMaxHeaders)
        ThrowFormat(std::string(context) + " are larger than this build reads");
    std::vector<std::uint8_t> bytes(stored);
    try
    {
        file_.ReadAt(offset, bytes.data(), bytes.size());
    }
    catch (const Error& error)
    {
        ThrowIo(std::string("Reading ") + context, error.what());
    }
    // nod decompresses into exactly the entries' size; what is not written stays zero.
    out.assign(count * entry, 0);
    try
    {
        decompressor_->Decompress(bytes.data(), bytes.size(), out.data(), out.size(), out.size());
    }
    catch (const Error& error)
    {
        ThrowIo(std::string("Decompressing ") + context, error.what());
    }
}

NodDiscMeta RvzReader::Meta() const
{
    NodDiscMeta meta{};
    meta.format = rvz_ ? NOD_FORMAT_RVZ : NOD_FORMAT_WIA;
    // nod's level types: unsigned for bzip2 and LZMA, signed for Zstandard.
    switch (compression_)
    {
    case Compression::None:
    case Compression::Purge: meta.compression = {NOD_COMPRESSION_KIND_NONE, 0}; break;
    case Compression::Bzip2: meta.compression = {NOD_COMPRESSION_KIND_BZIP2, std::uint8_t(level_)}; break;
    case Compression::Lzma: meta.compression = {NOD_COMPRESSION_KIND_LZMA, std::uint8_t(level_)}; break;
    case Compression::Lzma2: meta.compression = {NOD_COMPRESSION_KIND_LZMA2, std::uint8_t(level_)}; break;
    case Compression::Zstandard: meta.compression = {NOD_COMPRESSION_KIND_ZSTANDARD, std::int8_t(level_)}; break;
    }
    meta.block_size = chunk_size_;
    meta.decrypted = true;
    meta.needs_hash_recovery = true;
    meta.lossless = true;
    meta.disc_size = iso_size_;
    meta.crc32 = crc32_;
    std::memcpy(meta.md5, md5_, sizeof md5_);
    std::memcpy(meta.sha1, sha1_, sizeof sha1_);
    meta.xxh64 = xxh64_;
    return meta;
}

const RvzReader::Area* RvzReader::Find(std::uint32_t sector) const
{
    for (const Area& area : areas_)
        if (sector >= area.first_sector && sector < (area.end_offset + kSectorSize - 1) / kSectorSize) return &area;
    return nullptr;
}

RvzReader::GroupInfo RvzReader::Locate(const Area& area, std::uint32_t sector) const
{
    const std::uint32_t per_group = chunk_size_ / kSectorSize;
    const std::uint32_t relative = (sector - area.first_sector) / per_group;
    GroupInfo info{};
    info.index = area.group_index + relative;
    info.sector = area.first_sector + relative * per_group;
    info.partition = area.partition;
    if (area.partition)
    {
        info.sectors = std::min<std::uint32_t>(std::uint32_t(area.end_offset / kSectorSize) - info.sector, per_group);
        info.size = info.sectors * (kSectorSize - 0x400);
        // As nod: wrapping, from the start of the partition's data.
        info.section_offset = std::uint64_t(std::uint32_t(info.sector - area.partition_sector)) * (kSectorSize - 0x400);
    }
    else
    {
        info.size = std::uint32_t(std::min<std::uint64_t>(area.end_offset - std::uint64_t(info.sector) * kSectorSize,
                                                           chunk_size_));
        info.sectors = (info.size + kSectorSize - 1) / kSectorSize;
        info.section_offset = std::uint64_t(info.sector) * kSectorSize;
    }
    return info;
}

SectorKind RvzReader::Kind(std::uint32_t sector) const
{
    const Area* area = Find(sector);
    if (!area) return SectorKind::Missing;
    if (area->partition) return SectorKind::Decrypted;
    const GroupInfo info = Locate(*area, sector);
    return info.index < groups_.size() && groups_[info.index].size == 0 ? SectorKind::Zero : SectorKind::Data;
}

void RvzReader::ReadRawSector(std::uint32_t sector, std::uint8_t* out)
{
    const Area* area = Find(sector);
    if (!area || area->partition) throw Error(NOD_RESULT_ERR_IO, "Disc sector " + std::to_string(sector) + " is no raw data");
    const GroupInfo info = Locate(*area, sector);
    if (raw_group_index_ != info.index)
    {
        raw_group_index_ = UINT32_MAX;
        raw_group_.resize(chunk_size_);
        DecodeGroup(info, raw_group_.data());
        raw_group_index_ = info.index;
    }
    // The last raw data can end inside a sector: the rest reads as zeroes.
    const std::size_t within = std::size_t(sector - info.sector) * kSectorSize;
    const std::size_t count = std::min<std::size_t>(kSectorSize, info.size - within);
    std::memcpy(out, raw_group_.data() + within, count);
    std::memset(out + count, 0, kSectorSize - count);
}

const std::uint8_t* RvzReader::ReadDecryptedGroup(std::uint32_t sector, std::uint32_t& first, std::uint32_t& count)
{
    const Area* area = Find(sector);
    if (!area || !area->partition)
        throw Error(NOD_RESULT_ERR_IO, "Disc sector " + std::to_string(sector) + " is no partition data");
    const GroupInfo info = Locate(*area, sector);
    if (partition_group_index_ != info.index)
    {
        partition_group_index_ = UINT32_MAX;
        partition_group_.resize(info.size);
        DecodeGroup(info, partition_group_.data());
        partition_group_index_ = info.index;
    }
    first = info.sector;
    count = info.sectors;
    return partition_group_.data();
}

void RvzReader::DecodeGroup(const GroupInfo& info, std::uint8_t* out)
{
    const std::string context = "Reading WIA/RVZ group " + std::to_string(info.index);
    if (info.index >= groups_.size())
        throw Error(NOD_RESULT_ERR_IO, "Couldn't find WIA/RVZ group index " + std::to_string(info.index));
    const Group& group = groups_[info.index];
    // Zero size: all zeroes (and no exceptions).
    if (group.size == 0)
    {
        std::memset(out, 0, info.size);
        return;
    }
    if (group.size > 2 * max_decoded_ + 0x10000) ThrowIo(context, "the group is larger than its chunk");
    stored_.resize(group.size);
    try
    {
        file_.ReadAt(group.offset, stored_.data(), stored_.size());
        // Exception lists are stored as is (padded to four bytes) unless the group
        // is compressed with a real codec; then they lead its decompressed data.
        const bool stored_lists =
            compression_ == Compression::None || compression_ == Compression::Purge || !group.compressed;
        std::size_t start = info.partition && stored_lists ? SkipExceptionLists(stored_.data(), stored_.size(), chunk_size_, true) : 0;
        const std::uint8_t* data = stored_.data() + start;
        std::size_t size = stored_.size() - start;
        if (group.compressed)
        {
            std::size_t capacity = decompressor_->ContentSize(data, size);
            if (capacity > 2 * max_decoded_ + 0x10000) ThrowIo(context, "the group decompresses past its chunk");
            if (!capacity) capacity = max_decoded_;
            if (decompressed_.size() < capacity) decompressed_.resize(capacity);
            size = decompressor_->Decompress(data, size, decompressed_.data(), capacity, max_decoded_);
            data = decompressed_.data();
        }
        if (info.partition && !stored_lists)
        {
            start = SkipExceptionLists(data, size, chunk_size_, false);
            data += start;
            size -= start;
        }
        if (group.packed_size)
            Unpack(data, size, out, info);
        else if (size != info.size)
            throw Error(NOD_RESULT_ERR_IO, "WIA/RVZ group " + std::to_string(info.index) + " data size mismatch: "
                                               + std::to_string(size) + " != " + std::to_string(info.size));
        else
            std::memcpy(out, data, size);
    }
    catch (const Error& error)
    {
        ThrowIo(context, error.what());
    }
    // The disc's first 0x80 bytes come from the disc struct.
    if (info.sector == 0) std::memcpy(out, disc_head_, std::min<std::size_t>(sizeof disc_head_, info.size));
}

void RvzReader::Unpack(const std::uint8_t* data, std::size_t size, std::uint8_t* out, const GroupInfo& info)
{
    // Runs of data and of junk, each after its size; a set top bit marks junk,
    // which is the generator's output from a stored seed.
    std::size_t position = 0, written = 0;
    while (size - position >= 4)
    {
        const std::uint32_t entry = Read32(data + position);
        position += 4;
        const std::size_t count = entry & ~kCompressedBit, remaining = info.size - written;
        if (entry & kCompressedBit)
        {
            if (count > remaining)
                throw Error(NOD_RESULT_ERR_IO, "RVZ packed junk size too large: " + std::to_string(count) + " > "
                                                   + std::to_string(remaining));
            if (size - position < LaggedFibonacci::kSeedWords * 4) throw Error(NOD_RESULT_ERR_IO, "Filling LFG seed");
            std::uint32_t seed[LaggedFibonacci::kSeedWords];
            for (std::size_t word = 0; word < LaggedFibonacci::kSeedWords; ++word) seed[word] = Read32(data + position + 4 * word);
            position += sizeof seed;
            lfg_.Init(seed);
            // Junk restarts every 0x8000 bytes of the partition data (or disc).
            lfg_.Skip(std::size_t((info.section_offset + written) % kSectorSize));
            lfg_.Fill(out + written, count);
        }
        else
        {
            if (count > remaining)
                throw Error(NOD_RESULT_ERR_IO, "RVZ packed data size too large: " + std::to_string(count) + " > "
                                                   + std::to_string(remaining));
            if (size - position < count) throw Error(NOD_RESULT_ERR_IO, "RVZ packed data runs past its group");
            std::memcpy(out + written, data + position, count);
            position += count;
        }
        written += count;
    }
    if (written != info.size)
        throw Error(NOD_RESULT_ERR_IO, "RVZ packed data size mismatch: " + std::to_string(written) + " != "
                                           + std::to_string(info.size));
    if (position != size) throw Error(NOD_RESULT_ERR_IO, "Failed to consume all group data");
}
} // namespace mscharged::platform::wii_disc
