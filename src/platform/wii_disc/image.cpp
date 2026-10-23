#include "image.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace mscharged::platform::wii_disc
{
void ThrowFormat(const std::string& message) { throw Error(NOD_RESULT_ERR_FORMAT, "disc format error: " + message); }

void ThrowIo(const std::string& context, const std::string& cause)
{
    throw Error(NOD_RESULT_ERR_IO, context + ": " + cause);
}

bool IsValidUtf8(const char* text, std::size_t size)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text);
    for (std::size_t i = 0; i < size;)
    {
        const std::uint8_t lead = bytes[i];
        std::size_t length;
        std::uint32_t code;
        if (lead < 0x80)
        {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) length = 2, code = lead & 0x1F;
        else if (lead >= 0xE0 && lead <= 0xEF) length = 3, code = lead & 0x0F;
        else if (lead >= 0xF0 && lead <= 0xF4) length = 4, code = lead & 0x07;
        else return false;
        if (size - i < length) return false;
        for (std::size_t n = 1; n < length; ++n)
        {
            if ((bytes[i + n] & 0xC0) != 0x80) return false;
            code = code << 6 | (bytes[i + n] & 0x3F);
        }
        // No overlong forms, surrogates or code points past U+10FFFF.
        if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF))
            || (code >= 0xD800 && code <= 0xDFFF))
            return false;
        i += length;
    }
    return true;
}

namespace
{
constexpr std::uint8_t kWiiMagic[4] = {0x5D, 0x1C, 0x9E, 0xA3};
constexpr std::uint8_t kGcnMagic[4] = {0xC2, 0x33, 0x9F, 0x3D};
// WBFS block maps cover a dual-layer disc's sectors.
constexpr std::uint32_t kWbfsDiscSectors = 143432 * 2;

std::uint32_t Read32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

// As Rust shows an OS error.
std::string SystemError(int error) { return std::string(std::strerror(error)) + " (os error " + std::to_string(error) + ")"; }

bool IsRegularFile(const std::string& path, std::uint64_t& size)
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    size = std::uint64_t(info.st_size);
    return true;
}

// nod's split names, tried in this order; the first scheme with a part wins.
std::string SplitDotNumber(const std::string& path, unsigned index) { return path + "." + std::to_string(index); }

std::string SplitPartNumber(const std::string& path, unsigned index)
{
    const std::size_t slash = path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    const std::string name = path.substr(directory.size());
    // Rust's file_stem/extension: a leading dot does not start an extension.
    const std::size_t dot = name.rfind('.');
    std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string extension = dot == std::string::npos || dot == 0 ? "iso" : name.substr(dot + 1);
    // game.part0.iso names game.part1.iso, not game.part0.part1.iso.
    const std::size_t part = stem.rfind(".part");
    if (part != std::string::npos && part + 5 < stem.size()
        && std::all_of(stem.begin() + std::ptrdiff_t(part + 5), stem.end(),
                       [](char c) { return c >= '0' && c <= '9'; }))
        stem.erase(part);
    return directory + stem + ".part" + std::to_string(index) + "." + extension;
}

std::string SplitLastCharacter(const std::string& path, unsigned index)
{
    std::size_t end = path.size();
    if (end)
    {
        // Drops one character, not one byte, of a UTF-8 path.
        --end;
        while (end > 0 && (std::uint8_t(path[end]) & 0xC0) == 0x80) --end;
    }
    return path.substr(0, end) + std::to_string(index);
}
} // namespace

SplitFile::SplitFile(const std::string& path)
{
    std::uint64_t size = 0;
    struct stat info;
    if (stat(path.c_str(), &info) != 0) ThrowIo("Failed to open " + path, SystemError(errno));
    if (!S_ISREG(info.st_mode)) ThrowFormat("Input is not a file: " + path);
    parts_.push_back({path, 0, std::uint64_t(info.st_size), -1});
    size = std::uint64_t(info.st_size);
    for (const auto name : {SplitDotNumber, SplitPartNumber, SplitLastCharacter})
    {
        bool found = false;
        for (unsigned index = 1;; ++index)
        {
            const std::string part = name(path, index);
            if (part == path) continue;
            std::uint64_t part_size = 0;
            if (!IsRegularFile(part, part_size)) break;
            parts_.push_back({part, size, part_size, -1});
            size += part_size;
            found = true;
        }
        if (found) break;
    }
    for (Part& part : parts_)
    {
        part.fd = open(part.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (part.fd < 0)
        {
            const int error = errno;
            for (Part& opened : parts_)
                if (opened.fd >= 0) close(opened.fd);
            ThrowIo("Failed to open " + part.path, SystemError(error));
        }
    }
    size_ = size;
}

SplitFile::~SplitFile()
{
    for (const Part& part : parts_)
        if (part.fd >= 0) close(part.fd);
}

void SplitFile::ReadAt(std::uint64_t offset, void* out, std::size_t size) const
{
    auto* destination = static_cast<std::uint8_t*>(out);
    while (size)
    {
        const auto part = std::find_if(parts_.begin(), parts_.end(), [offset](const Part& p) {
            return offset >= p.begin && offset - p.begin < p.size;
        });
        if (part == parts_.end()) ThrowIo("Reading the image at " + std::to_string(offset), "unexpected end of the file");
        const std::uint64_t within = offset - part->begin;
        const std::size_t count = std::size_t(std::min<std::uint64_t>(size, part->size - within));
        // newlib has no pread; callers serialize reads.
        if (lseek(part->fd, off_t(within), SEEK_SET) < 0) ThrowIo("Reading " + part->path, SystemError(errno));
        std::size_t done = 0;
        while (done < count)
        {
            const ssize_t got = read(part->fd, destination + done, count - done);
            if (got < 0)
            {
                if (errno == EINTR) continue;
                ThrowIo("Reading " + part->path, SystemError(errno));
            }
            if (got == 0) ThrowIo("Reading " + part->path, "unexpected end of the file");
            done += std::size_t(got);
        }
        destination += count;
        offset += count;
        size -= count;
    }
}

Image::Image(const std::string& path) : file_(path)
{
    std::uint8_t start[0x20];
    if (file_.Size() < sizeof start) ThrowFormat("Unknown disc format");
    file_.ReadAt(0, start, sizeof start);
    struct Unsupported
    {
        const char* magic;
        const char* name;
    };
    for (const Unsupported& format : {Unsupported{"CISO", "CISO"}, Unsupported{"\x01\xC0\x0B\xB1", "GCZ"},
                                      Unsupported{"EGGS", "NFS"}, Unsupported{"\xAE\x0F\x38\xA2", "TGC"},
                                      Unsupported{"WIA\x01", "WIA"}, Unsupported{"RVZ\x01", "RVZ"}})
        if (std::memcmp(start, format.magic, 4) == 0)
            ThrowFormat(std::string(format.name) + " images are not supported on this build; use ISO or WBFS");
    if (std::memcmp(start, "WBFS", 4) == 0)
    {
        format_ = NOD_FORMAT_WBFS;
        const std::uint32_t sectors = Read32(start + 4);
        const unsigned sector_shift = start[8], block_shift = start[9];
        if (sector_shift < 9 || sector_shift > 16) ThrowFormat("Invalid WBFS sector size shift " + std::to_string(sector_shift));
        if (block_shift < 15 || block_shift > 30)
            ThrowFormat("Unsupported WBFS block size shift " + std::to_string(block_shift));
        const std::uint32_t sector_size = 1u << sector_shift;
        block_size_ = 1u << block_shift;
        const std::uint64_t expected = std::uint64_t(sectors) * sector_size;
        if (file_.Size() != expected)
            ThrowFormat("Invalid WBFS file size: " + std::to_string(file_.Size()) + ", expected "
                        + std::to_string(expected));
        std::vector<std::uint8_t> discs(sector_size - 12);
        file_.ReadAt(12, discs.data(), discs.size());
        if (discs[0] != 1) ThrowFormat("WBFS doesn't contain a disc");
        if (std::any_of(discs.begin() + 1, discs.end(), [](std::uint8_t used) { return used != 0; }))
            ThrowFormat("Only single WBFS discs are supported");
        // The disc's entry: a copy of its first 0x100 bytes, then the map of
        // its blocks to image blocks.
        std::vector<std::uint8_t> map(std::size_t(kWbfsDiscSectors >> (block_shift - 15)) * 2);
        try
        {
            file_.ReadAt(sector_size + 0x100, map.data(), map.size());
        }
        catch (const Error& error)
        {
            ThrowIo("Reading WBFS LBA table", error.what());
        }
        block_map_.resize(map.size() / 2);
        for (std::size_t i = 0; i < block_map_.size(); ++i)
            block_map_[i] = std::uint16_t(map[2 * i] << 8 | map[2 * i + 1]);
        return;
    }
    if (std::memcmp(start + 0x18, kWiiMagic, 4) == 0 || std::memcmp(start + 0x1C, kGcnMagic, 4) == 0)
    {
        format_ = NOD_FORMAT_ISO;
        return;
    }
    ThrowFormat("Unknown disc format");
}

SectorKind Image::Kind(std::uint32_t sector) const
{
    const std::uint64_t offset = std::uint64_t(sector) * kSectorSize;
    if (format_ == NOD_FORMAT_ISO) return offset < file_.Size() ? SectorKind::Data : SectorKind::Missing;
    const std::uint64_t block = offset / block_size_;
    if (block >= block_map_.size()) return SectorKind::Missing;
    return block_map_[std::size_t(block)] ? SectorKind::Data : SectorKind::Zero;
}

std::uint64_t Image::Offset(std::uint32_t sector) const
{
    const std::uint64_t offset = std::uint64_t(sector) * kSectorSize;
    if (format_ == NOD_FORMAT_ISO) return offset;
    return std::uint64_t(block_map_[std::size_t(offset / block_size_)]) * block_size_ + offset % block_size_;
}

void Image::ReadSectors(std::uint32_t first, std::uint32_t count, std::uint8_t* out, SectorKind* kinds) const
{
    std::uint32_t i = 0;
    while (i < count)
    {
        kinds[i] = Kind(first + i);
        if (kinds[i] != SectorKind::Data)
        {
            std::memset(out + std::size_t(i) * kSectorSize, 0, kSectorSize);
            ++i;
            continue;
        }
        // One read for each run of sectors that are adjacent in the image.
        const std::uint64_t offset = Offset(first + i);
        std::uint32_t run = 1;
        while (i + run < count && Kind(first + i + run) == SectorKind::Data
               && Offset(first + i + run) == offset + std::uint64_t(run) * kSectorSize)
        {
            kinds[i + run] = SectorKind::Data;
            ++run;
        }
        std::uint8_t* destination = out + std::size_t(i) * kSectorSize;
        std::size_t size = std::size_t(run) * kSectorSize;
        if (format_ == NOD_FORMAT_ISO && offset + size > file_.Size())
        {
            // An ISO that ends inside its last sector: the rest reads as zeroes.
            const std::size_t present = std::size_t(file_.Size() - offset);
            std::memset(destination + present, 0, size - present);
            size = present;
        }
        file_.ReadAt(offset, destination, size);
        i += run;
    }
}
} // namespace mscharged::platform::wii_disc
