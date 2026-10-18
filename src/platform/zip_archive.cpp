#include "platform/zip_archive.h"

#include "platform/path.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace mscharged::platform
{
namespace
{
constexpr std::uint32_t kEndRecord = 0x06054b50;
constexpr std::uint32_t kEnd64Locator = 0x07064b50;
constexpr std::uint32_t kEnd64Record = 0x06064b50;
constexpr std::uint32_t kDirectoryEntry = 0x02014b50;
constexpr std::uint32_t kLocalHeader = 0x04034b50;
// More entries than any texture pack has; a damaged count asks for more.
constexpr std::uint64_t kMaxEntries = 4'000'000;
constexpr std::uint64_t kMaxDirectory = 1ull << 30;

std::uint16_t Read16(const unsigned char* data) { return std::uint16_t(data[0] | data[1] << 8); }
std::uint32_t Read32(const unsigned char* data) { return std::uint32_t(Read16(data)) | std::uint32_t(Read16(data + 2)) << 16; }
std::uint64_t Read64(const unsigned char* data) { return std::uint64_t(Read32(data)) | std::uint64_t(Read32(data + 4)) << 32; }

[[noreturn]] void Damaged(const std::filesystem::path& path, const char* what)
{
    throw std::runtime_error(PathUtf8(path.filename()) + " is not a readable zip archive (" + what + ")");
}

bool ReadAt(std::ifstream& file, std::uint64_t offset, void* data, std::size_t size)
{
    file.clear();
    file.seekg(std::streamoff(offset));
    return bool(file.read(static_cast<char*>(data), std::streamsize(size)));
}
}

ZipArchive::ZipArchive(std::filesystem::path path) : path_(std::move(path))
{
    std::ifstream file(path_, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + PathUtf8(path_));
    file.seekg(0, std::ios::end);
    length_ = std::uint64_t(file.tellg());
    // The end-of-central-directory record: 22 bytes plus up to 64 KiB of comment.
    const std::uint64_t tail = std::min<std::uint64_t>(length_, 22 + 65535);
    if (tail < 22) Damaged(path_, "too short");
    std::vector<unsigned char> end(tail);
    if (!ReadAt(file, length_ - tail, end.data(), end.size())) Damaged(path_, "truncated");
    std::optional<std::size_t> record;
    for (std::size_t at = tail - 22;; --at)
    {
        if (Read32(end.data() + at) == kEndRecord) { record = at; break; }
        if (at == 0) break;
    }
    if (!record) Damaged(path_, "no central directory");
    const unsigned char* eocd = end.data() + *record;
    if (Read16(eocd + 4) || Read16(eocd + 6)) Damaged(path_, "split archive");
    std::uint64_t count = Read16(eocd + 10), directory_size = Read32(eocd + 12), directory_at = Read32(eocd + 16);
    if (count == 0xFFFF || directory_size == 0xFFFFFFFFu || directory_at == 0xFFFFFFFFu)
    {
        // zip64: the locator just before the record points to the zip64 record.
        const std::uint64_t record_at = length_ - tail + *record;
        unsigned char locator[20];
        if (record_at < 20 || !ReadAt(file, record_at - 20, locator, sizeof locator) || Read32(locator) != kEnd64Locator)
            Damaged(path_, "zip64 locator");
        unsigned char record64[56];
        const std::uint64_t record64_at = Read64(locator + 8);
        if (record64_at > length_ || !ReadAt(file, record64_at, record64, sizeof record64)
            || Read32(record64) != kEnd64Record)
            Damaged(path_, "zip64 record");
        count = Read64(record64 + 32);
        directory_size = Read64(record64 + 40);
        directory_at = Read64(record64 + 48);
    }
    if (count > kMaxEntries || directory_size > kMaxDirectory || directory_at > length_
        || directory_size > length_ - directory_at)
        Damaged(path_, "central directory");
    std::vector<unsigned char> directory(directory_size);
    if (!directory.empty() && !ReadAt(file, directory_at, directory.data(), directory.size()))
        Damaged(path_, "truncated");
    entries_.reserve(std::size_t(count));
    std::size_t at = 0;
    for (std::uint64_t n = 0; n < count; ++n)
    {
        if (at + 46 > directory.size() || Read32(directory.data() + at) != kDirectoryEntry)
            Damaged(path_, "central directory");
        const unsigned char* item = directory.data() + at;
        const std::size_t name_length = Read16(item + 28), extra_length = Read16(item + 30),
                          comment_length = Read16(item + 32);
        if (at + 46 + name_length + extra_length + comment_length > directory.size())
            Damaged(path_, "central directory");
        Entry entry;
        const std::uint16_t flags = Read16(item + 8);
        entry.method = Read16(item + 10);
        entry.crc = Read32(item + 16);
        entry.packed = Read32(item + 20);
        entry.size = Read32(item + 24);
        entry.local_header = Read32(item + 42);
        entry.name.assign(reinterpret_cast<const char*>(item + 46), name_length);
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        // zip64 extra field: the 64-bit values of the fields set to all ones, in this order.
        const unsigned char* extra = item + 46 + name_length;
        for (std::size_t field = 0; field + 4 <= extra_length;)
        {
            const std::uint16_t id = Read16(extra + field), size = Read16(extra + field + 2);
            if (field + 4 + size > extra_length) break;
            if (id == 0x0001)
            {
                const unsigned char* value = extra + field + 4;
                std::size_t left = size;
                auto take = [&](std::uint64_t& target) {
                    if (target != 0xFFFFFFFFu) return;
                    if (left < 8) Damaged(path_, "zip64 entry");
                    target = Read64(value);
                    value += 8;
                    left -= 8;
                };
                take(entry.size);
                take(entry.packed);
                take(entry.local_header);
            }
            field += 4 + size;
        }
        at += 46 + name_length + extra_length + comment_length;
        const bool folder = !entry.name.empty() && entry.name.back() == '/';
        entry.readable = !folder && !(flags & 1) && (entry.method == 0 || entry.method == 8)
            && entry.size <= kMaxEntrySize && entry.local_header < length_ && entry.packed <= length_;
        if (folder || entry.name.empty()) continue;
        index_.emplace(entry.name, entries_.size());
        entries_.push_back(std::move(entry));
    }
}

const ZipArchive::Entry* ZipArchive::Find(std::string_view name) const
{
    const auto it = index_.find(std::string(name));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

bool ZipArchive::Read(const Entry& entry, std::vector<std::uint8_t>& out) const
{
    out.clear();
    if (!entry.readable) return false;
    std::ifstream file(path_, std::ios::binary);
    unsigned char local[30];
    if (!file || !ReadAt(file, entry.local_header, local, sizeof local) || Read32(local) != kLocalHeader) return false;
    const std::uint64_t data_at = entry.local_header + 30 + Read16(local + 26) + Read16(local + 28);
    if (data_at > length_ || entry.packed > length_ - data_at) return false;
    try
    {
        out.resize(std::size_t(entry.size));
        if (entry.method == 0)
        {
            if (entry.packed != entry.size || (entry.size && !ReadAt(file, data_at, out.data(), out.size())))
            {
                out.clear();
                return false;
            }
        }
        else
        {
            std::vector<unsigned char> packed(std::size_t(entry.packed));
            if (!packed.empty() && !ReadAt(file, data_at, packed.data(), packed.size()))
            {
                out.clear();
                return false;
            }
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
            {
                out.clear();
                return false;
            }
            int result = Z_OK;
            std::uint64_t consumed = 0, produced = 0;
            // zlib counts in uInt: feed and drain in pieces of at most 1 GiB.
            constexpr std::uint64_t kStep = 1u << 30;
            while (result == Z_OK)
            {
                stream.next_in = packed.data() + consumed;
                stream.avail_in = uInt(std::min<std::uint64_t>(packed.size() - consumed, kStep));
                stream.next_out = out.data() + produced;
                stream.avail_out = uInt(std::min<std::uint64_t>(out.size() - produced, kStep));
                const uInt in_before = stream.avail_in, out_before = stream.avail_out;
                result = inflate(&stream, Z_NO_FLUSH);
                consumed += in_before - stream.avail_in;
                produced += out_before - stream.avail_out;
                if (result == Z_BUF_ERROR || (result == Z_OK && in_before == stream.avail_in
                                              && out_before == stream.avail_out))
                    break;
            }
            inflateEnd(&stream);
            if (result != Z_STREAM_END || produced != entry.size)
            {
                out.clear();
                return false;
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        out.clear();
        return false;
    }
    uLong check = crc32(0, nullptr, 0);
    for (std::size_t done = 0; done < out.size();)
    {
        const std::size_t step = std::min<std::size_t>(out.size() - done, 1u << 30);
        check = crc32(check, out.data() + done, uInt(step));
        done += step;
    }
    if (std::uint32_t(check) != entry.crc)
    {
        out.clear();
        return false;
    }
    return true;
}
}
