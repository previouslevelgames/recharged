#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mscharged::platform
{
// Reads single files out of a zip archive without unpacking it: stored and
// deflated entries, zip64 included. Opening reads only the central directory;
// Read opens the file again each time, so any number of threads may read at
// once.
class ZipArchive
{
public:
    struct Entry
    {
        std::string name; // '/'-separated, as stored (a '\' from Windows tools becomes '/')
        std::uint64_t size = 0;
        std::uint64_t packed = 0;
        std::uint64_t local_header = 0;
        std::uint32_t crc = 0;
        std::uint16_t method = 0;
        bool readable = false; // stored or deflated, not encrypted, not a folder
    };

    // Throws std::runtime_error when the file is not a zip archive or is damaged.
    explicit ZipArchive(std::filesystem::path path);

    const std::filesystem::path& Path() const { return path_; }
    const std::vector<Entry>& Entries() const { return entries_; }
    // The entry named `name` (exact spelling), or null.
    const Entry* Find(std::string_view name) const;
    // The entry's contents, checked against its size and CRC. False (and
    // `out` empty) when the archive changed, is damaged or the entry cannot
    // be read.
    bool Read(const Entry& entry, std::vector<std::uint8_t>& out) const;

    // Larger entries are refused (a damaged size field would ask for it).
    static constexpr std::uint64_t kMaxEntrySize = 1ull << 30;

private:
    std::filesystem::path path_;
    std::uint64_t length_ = 0;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
};
}
