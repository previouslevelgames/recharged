#pragma once

#include <nod.h>

#include <cstddef>
#include <cstdint>
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
    Data,    // in the image
    Zero,    // left out of a WBFS: reads as zeroes
    Missing, // past the end of the image
};

// A Wii disc image as disc sectors: an ISO, or a WBFS (one disc, its block map).
// Other formats fail with ERR_FORMAT. Reads are not thread-safe.
class Image
{
public:
    explicit Image(const std::string& path);

    NodFormat Format() const { return format_; }
    // The WBFS block size, 0 for an ISO.
    std::uint32_t BlockSize() const { return block_size_; }
    // The ISO's size; 0 for a WBFS, which does not record the disc's size.
    std::uint64_t IsoSize() const { return format_ == NOD_FORMAT_ISO ? file_.Size() : 0; }

    SectorKind Kind(std::uint32_t sector) const;
    // Reads `count` disc sectors into `out` (count * kSectorSize bytes) and
    // their kinds; Zero and Missing sectors read as zeroes, and so does the
    // part of the last ISO sector past the end of the file.
    void ReadSectors(std::uint32_t first, std::uint32_t count, std::uint8_t* out, SectorKind* kinds) const;

private:
    // The image offset of a Data sector.
    std::uint64_t Offset(std::uint32_t sector) const;

    SplitFile file_;
    NodFormat format_ = NOD_FORMAT_ISO;
    std::uint32_t block_size_ = 0;
    std::vector<std::uint16_t> block_map_; // WBFS: disc block -> image block (0: left out)
};
} // namespace mscharged::platform::wii_disc
