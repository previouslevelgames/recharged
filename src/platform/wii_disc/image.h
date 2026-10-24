#pragma once

#include <nod.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace mscharged::platform::wii_disc
{
constexpr std::uint32_t kSectorSize = 0x8000;

// An error for the C API: its NodResult and the message nod_error_message()
// returns. Never crosses the C API.
class Error : public std::runtime_error
{
public:
    // A message with a NUL byte cannot be a C string; nod says so instead.
    Error(NodResult result, const std::string& message)
        : std::runtime_error(message.find('\0') == std::string::npos ? message : "(error contained null byte)"),
          result_(result)
    {
    }
    NodResult Result() const { return result_; }

private:
    NodResult result_;
};

// nod's wording: "disc format error: ..." and "<context>: <cause>".
[[noreturn]] void ThrowFormat(const std::string& message);
[[noreturn]] void ThrowIo(const std::string& context, const std::string& cause);

// nod takes paths and certificate issuers as Rust strings, which must be UTF-8.
bool IsValidUtf8(const char* text, std::size_t size);

// One image file, or its split parts read as one: nod's names path.1, path.2
// ...; stem.part1.ext ...; and the path with its last character replaced by
// 1, 2 ... (game.wbfs, game.wbf1). Reads are not thread-safe.
class SplitFile
{
public:
    explicit SplitFile(const std::string& path);
    ~SplitFile();
    SplitFile(const SplitFile&) = delete;
    SplitFile& operator=(const SplitFile&) = delete;

    std::uint64_t Size() const { return size_; }
    std::size_t PartCount() const { return parts_.size(); }
    // Reads exactly `size` bytes or throws (also past the end).
    void ReadAt(std::uint64_t offset, void* out, std::size_t size) const;

private:
    struct Part
    {
        std::string path;
        std::uint64_t begin;
        std::uint64_t size;
        int fd;
    };
    std::vector<Part> parts_;
    std::uint64_t size_ = 0;
};

// How a disc sector is stored in the image.
enum class SectorKind : std::uint8_t
{
    Data,      // in the image
    Zero,      // left out of a WBFS, or an all-zero WIA/RVZ group: reads as zeroes
    Missing,   // past the end of the image
    Decrypted, // WIA/RVZ partition data, decrypted without hashes: ReadDecryptedGroup
};

class RvzReader;

// A Wii disc image as disc sectors: an ISO, a WBFS (one disc, its block map),
// or a WIA or RVZ. Other formats fail with ERR_FORMAT. Reads are not thread-safe.
class Image
{
public:
    explicit Image(const std::string& path);
    ~Image();

    NodFormat Format() const { return format_; }
    // What nod's DiscMeta says of the image's format.
    NodDiscMeta Meta() const;
    // The disc's size as the image records it; 0 for a WBFS, which does not.
    std::uint64_t DiscSize() const;
    // Sectors a WIA/RVZ decodes at once; 0 for other formats.
    std::uint32_t GroupSectors() const;

    SectorKind Kind(std::uint32_t sector) const;
    // Reads `count` disc sectors into `out` (count * kSectorSize bytes) and
    // their kinds; Zero, Missing and Decrypted sectors read as zeroes, and so
    // does the part of the last ISO sector past the end of the file.
    void ReadSectors(std::uint32_t first, std::uint32_t count, std::uint8_t* out, SectorKind* kinds);
    // The WIA/RVZ group of decrypted partition data that holds `sector`: its
    // first sector, its sector count and its data (0x7C00 bytes a sector),
    // valid until the next read.
    const std::uint8_t* ReadDecryptedGroup(std::uint32_t sector, std::uint32_t& first, std::uint32_t& count);

private:
    // The image offset of a Data sector.
    std::uint64_t Offset(std::uint32_t sector) const;

    SplitFile file_;
    NodFormat format_ = NOD_FORMAT_ISO;
    std::uint32_t block_size_ = 0;
    std::vector<std::uint16_t> block_map_; // WBFS: disc block -> image block (0: left out)
    std::unique_ptr<RvzReader> rvz_;
};
} // namespace mscharged::platform::wii_disc
