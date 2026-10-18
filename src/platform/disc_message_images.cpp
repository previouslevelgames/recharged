#include "platform/disc_message_images.h"

#include "platform/sha1.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace mscharged
{
namespace
{
// The decompilation's tools/extract_disc_messages.py: LANGUAGES, IMAGE_TYPES.
constexpr const char* kLanguages[kDiscMessageLanguages] = {"eng", "fre", "deu", "spa", "ita", "jpn", "uke"};
constexpr const char* kTypes[kDiscMessageTypes] = {"FatalErrorImage", "NoDiskImage", "WrongDiskImage", "RetryImage",
                                                   "Loading"};
constexpr std::size_t kDolHeaderSize = 0x100;
constexpr std::size_t kTextSections = 7;
constexpr std::size_t kSections = 18; // 7 text, then 11 data sections
constexpr std::size_t kEntrySize = 16;  // BE width, height, compressed size, address

std::uint32_t Be32(const std::uint8_t* bytes)
{
    return std::uint32_t(bytes[0]) << 24 | std::uint32_t(bytes[1]) << 16 | std::uint32_t(bytes[2]) << 8 | bytes[3];
}

std::string Hex(std::uint32_t value)
{
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

// A DOL's section table, for reading loaded addresses back from the file.
class Dol
{
public:
    Dol(const std::uint8_t* data, std::size_t size) : data_(data)
    {
        if (size < kDolHeaderSize) throw std::runtime_error("main.dol is shorter than its header");
        for (std::size_t i = 0; i < kSections; ++i)
        {
            Section& section = sections_[i];
            section.offset = Be32(data + i * 4);
            section.address = Be32(data + 0x48 + i * 4);
            section.length = Be32(data + 0x90 + i * 4);
            if (section.length
                && (section.offset < kDolHeaderSize || std::uint64_t(section.offset) + section.length > size))
                throw std::runtime_error("main.dol section " + std::to_string(i) + " lies outside the file");
        }
    }

    // The bytes at a loaded address, from one data section, as the game reads them.
    const std::uint8_t* Data(std::uint32_t address, std::uint32_t length) const
    {
        for (std::size_t i = kTextSections; i < kSections; ++i)
        {
            const Section& section = sections_[i];
            if (section.length && address >= section.address
                && std::uint64_t(address) + length <= std::uint64_t(section.address) + section.length)
                return data_ + section.offset + (address - section.address);
        }
        return nullptr;
    }

private:
    struct Section
    {
        std::uint32_t offset = 0;
        std::uint32_t address = 0;
        std::uint32_t length = 0;
    };
    const std::uint8_t* data_;
    Section sections_[kSections];
};

// How many bytes the game's UncompressLidMessage writes for this input: a
// zero byte and a count stand for that many zeros, any other byte for
// itself, and the last byte is always written as is.
std::uint64_t UnpackedSize(const std::uint8_t* data, std::uint32_t size)
{
    std::uint64_t written = 0;
    for (std::uint32_t i = 0; i + 1 < size; ++i)
    {
        if (data[i] == 0)
            written += data[++i];
        else
            ++written;
    }
    return written + 1;
}
} // namespace

std::string DiscMessageImageName(std::size_t index)
{
    if (index >= kDiscMessageImageCount) throw std::out_of_range("No disc-message image " + std::to_string(index));
    return std::string("HEADER_") + kTypes[index % kDiscMessageTypes] + "_" + kLanguages[index / kDiscMessageTypes];
}

std::vector<DiscMessageImage> ExtractDiscMessageImages(const std::uint8_t* dol, std::size_t size,
                                                       std::uint32_t table_address)
{
    const Dol executable(dol, size);
    const std::uint8_t* table = executable.Data(table_address, kDiscMessageImageCount * kEntrySize);
    if (!table) throw std::runtime_error("The disc-message table " + Hex(table_address) + " is not in a data section");
    std::vector<DiscMessageImage> images;
    images.reserve(kDiscMessageImageCount);
    for (std::size_t i = 0; i < kDiscMessageImageCount; ++i)
    {
        const std::uint8_t* entry = table + i * kEntrySize;
        const std::uint32_t width = Be32(entry), height = Be32(entry + 4);
        const std::uint32_t length = Be32(entry + 8), address = Be32(entry + 12);
        const std::string name = DiscMessageImageName(i);
        if (!width || width % 8 || !height || !length)
            throw std::runtime_error("Invalid disc-message image " + name + " (" + std::to_string(width) + "x"
                                     + std::to_string(height) + ", " + std::to_string(length) + " bytes)");
        const std::uint8_t* bytes = executable.Data(address, length);
        if (!bytes) throw std::runtime_error("Disc-message image " + name + " at " + Hex(address) + " is not in a data section");
        // The game unpacks into exactly width * height / 8 bytes without bounds checks.
        const std::uint64_t unpacked = UnpackedSize(bytes, length);
        if (unpacked != std::uint64_t(width) * height / 8)
            throw std::runtime_error("Disc-message image " + name + " unpacks to " + std::to_string(unpacked)
                                     + " bytes instead of " + std::to_string(std::uint64_t(width) * height / 8));
        images.push_back({width, height, std::vector<std::uint8_t>(bytes, bytes + length)});
    }
    return images;
}

std::vector<DiscMessageImage> ExtractDiscMessageImages(const std::uint8_t* dol, std::size_t size,
                                                       std::string_view dol_sha1, std::uint32_t table_address)
{
    const std::string hash = platform::Sha1Hex(dol, size);
    if (hash != dol_sha1) throw std::runtime_error("main.dol has SHA-1 " + hash + ", expected " + std::string(dol_sha1));
    return ExtractDiscMessageImages(dol, size, table_address);
}

DiscMessages ReadDiscMessages(const std::filesystem::path& disc, const GameRelease& release)
{
    const std::vector<std::uint8_t> dol = ReadDiscMainDol(disc);
    DiscMessages found;
    found.dol_sha1 = platform::Sha1Hex(dol.data(), dol.size());
    found.modified = found.dol_sha1 != release.dol_sha1;
    try
    {
        found.images = ExtractDiscMessageImages(dol.data(), dol.size(), release.disc_message_table);
    }
    catch (const std::runtime_error& error)
    {
        if (!found.modified)
            throw std::runtime_error("The " + ReleaseLabel(release) + " main.dol has no readable disc-message images ("
                                     + error.what() + ")");
        found.missing = error.what();
    }
    return found;
}

void InstallDiscMessageImages(const std::vector<DiscMessageImage>& images, DiscMessageImageStorage storage)
{
    if (images.size() != kDiscMessageImageCount)
        throw std::runtime_error("Expected " + std::to_string(kDiscMessageImageCount) + " disc-message images, got "
                                 + std::to_string(images.size()));
    unsigned char* targets[kDiscMessageImageCount];
    for (unsigned int i = 0; i < kDiscMessageImageCount; ++i)
    {
        unsigned long size = 0;
        int width = 0, height = 0;
        targets[i] = storage(i, &size, &width, &height);
        const DiscMessageImage& image = images[i];
        const std::string name = DiscMessageImageName(i);
        if (!targets[i]) throw std::runtime_error("The game module has no storage for disc-message image " + name);
        if (size != image.data.size() || width != int(image.width) || height != int(image.height))
            throw std::runtime_error("The game module's " + name + " is " + std::to_string(width) + "x"
                                     + std::to_string(height) + " in " + std::to_string(size) + " bytes; main.dol's is "
                                     + std::to_string(image.width) + "x" + std::to_string(image.height) + " in "
                                     + std::to_string(image.data.size()) + " bytes");
    }
    unsigned long size = 0;
    int width = 0, height = 0;
    if (storage(kDiscMessageImageCount, &size, &width, &height))
        throw std::runtime_error("The game module has more disc-message images than main.dol's table");
    for (std::size_t i = 0; i < kDiscMessageImageCount; ++i)
        std::memcpy(targets[i], images[i].data.data(), images[i].data.size());
}

std::vector<std::uint8_t> BlankDiscMessageImage(std::size_t packed_size, std::uint32_t width, std::uint32_t height)
{
    // Zero runs as (0, count) pairs from the start; the last byte, which the
    // game always writes as is, stays 0 (with an even size it is also the
    // count of a last, empty pair).
    const std::uint64_t zeros = std::uint64_t(width) * height / 8;
    const std::size_t pairs = packed_size ? (packed_size - 1) / 2 : 0;
    if (!zeros || !packed_size || zeros - 1 > std::uint64_t(pairs) * 255)
        throw std::runtime_error("No empty " + std::to_string(width) + "x" + std::to_string(height)
                                 + " picture fits in " + std::to_string(packed_size) + " bytes");
    std::vector<std::uint8_t> packed(packed_size, 0);
    std::uint64_t left = zeros - 1;
    for (std::size_t pair = 0; pair < pairs && left; ++pair)
    {
        const auto count = std::uint8_t(left < 255 ? left : 255);
        packed[2 * pair + 1] = count;
        left -= count;
    }
    return packed;
}

std::string InstallDiscMessages(const DiscMessages& found, DiscMessageImageStorage storage)
{
    const std::string count = std::to_string(kDiscMessageImageCount);
    if (!found.modified)
    {
        InstallDiscMessageImages(found.images, storage);
        return "Disc messages: " + count + " images from the disc's main.dol";
    }
    const std::string modified = "the disc's main.dol is modified (SHA-1 " + found.dol_sha1 + ")";
    std::string reason = found.missing;
    if (reason.empty())
    {
        try
        {
            InstallDiscMessageImages(found.images, storage);
            return "Disc messages: " + count + " images; " + modified + ", its disc messages are intact";
        }
        catch (const std::runtime_error& error)
        {
            reason = error.what();
        }
    }
    // Empty pictures of the module's own sizes, through the same checks.
    std::vector<DiscMessageImage> blanks;
    for (unsigned int i = 0; i < kDiscMessageImageCount; ++i)
    {
        unsigned long size = 0;
        int width = 0, height = 0;
        if (!storage(i, &size, &width, &height) || width <= 0 || height <= 0)
            throw std::runtime_error("The game module has no storage for disc-message image " + DiscMessageImageName(i));
        blanks.push_back({std::uint32_t(width), std::uint32_t(height),
                          BlankDiscMessageImage(size, std::uint32_t(width), std::uint32_t(height))});
    }
    InstallDiscMessageImages(blanks, storage);
    return "Disc messages: blank; " + modified + " without usable disc messages (" + reason
        + "), so the disc error and loading screens stay empty";
}
} // namespace mscharged
