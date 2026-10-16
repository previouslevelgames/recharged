// Disc-message images from main.dol: SHA-1, DOL/table parsing, the copy into
// the game module's arrays. Synthetic data only; with --disc it reads an
// image through nod and prints sizes and hashes (no image bytes).
#include "platform/disc_message_images.h"
#include "platform/game_release.h"
#include "platform/sha1.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace mscharged;
using Bytes = std::vector<std::uint8_t>;

int checks = 0;

void Require(bool condition, const std::string& message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void RequireThrows(const std::function<void()>& action, const std::string& expected, const std::string& message)
{
    ++checks;
    try
    {
        action();
    }
    catch (const std::runtime_error& error)
    {
        if (std::string(error.what()).find(expected) == std::string::npos)
            throw std::runtime_error(message + ": unexpected error \"" + error.what() + "\"");
        return;
    }
    throw std::runtime_error(message + ": no error");
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) bytes[offset + i] = std::uint8_t(value >> (24 - 8 * i));
}

void TestSha1()
{
    Require(platform::Sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 of nothing");
    Require(platform::Sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of abc");
    Require(platform::Sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
                == "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
            "SHA-1 of the 448-bit vector");
    Require(platform::Sha1Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrl"
                              "mnopqrsmnopqrstnopqrstu")
                == "a49b2446a02c645bf419f995b67091253a04a259",
            "SHA-1 of the 896-bit vector");
    Require(platform::Sha1Hex("The quick brown fox jumps over the lazy dog") == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
            "SHA-1 of the fox");
    // A million 'a', fed in uneven pieces across block boundaries.
    platform::Sha1 hash;
    const std::string piece(997, 'a');
    std::size_t left = 1000000;
    while (left)
    {
        const std::size_t take = left < piece.size() ? left : piece.size();
        hash.Update(piece.data(), take);
        left -= take;
    }
    Require(hash.FinishHex() == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA-1 of a million a");
}

// The game's packing: a zero byte plus a count stand for that many zeros, the
// last byte is always stored as is.
Bytes Pack(const Bytes& raw)
{
    Bytes packed;
    std::size_t i = 0;
    while (i + 1 < raw.size())
    {
        if (raw[i] == 0)
        {
            std::size_t run = 0;
            while (i + run + 1 < raw.size() && raw[i + run] == 0 && run < 255) ++run;
            packed.push_back(0);
            packed.push_back(std::uint8_t(run));
            i += run;
        }
        else
            packed.push_back(raw[i++]);
    }
    packed.push_back(raw.back());
    return packed;
}

// The game's UncompressLidMessage, for checking Pack.
Bytes Unpack(const Bytes& packed, std::size_t size)
{
    Bytes raw(size + 256);
    std::size_t index = 0;
    for (std::size_t i = 0; i + 1 < packed.size(); ++i)
    {
        if (packed[i] == 0)
        {
            for (int k = 0; k < packed[i + 1]; ++k) raw[index++] = 0;
            ++i;
        }
        else
            raw[index++] = packed[i];
    }
    raw[index++] = packed.back();
    raw.resize(index);
    return raw;
}

struct SyntheticDol
{
    Bytes dol;
    std::vector<DiscMessageImage> images;
    std::uint32_t table = 0x80200010u;
    std::size_t table_offset = 0;
};

constexpr std::uint32_t kImageBase = 0x80100000u;

// One text section, the images in data section 0, the table 16 bytes into
// data section 1.
SyntheticDol MakeDol()
{
    SyntheticDol result;
    std::vector<Bytes> packed;
    std::uint32_t images_size = 0;
    for (std::size_t i = 0; i < kDiscMessageImageCount; ++i)
    {
        const std::uint32_t width = 8 * std::uint32_t(2 + i % 4), height = std::uint32_t(3 + i % 5);
        Bytes raw(width * height / 8);
        for (std::size_t n = 0; n < raw.size(); ++n)
            raw[n] = (n * 7 + i) % 5 == 0 ? std::uint8_t(0x81 + i) : 0;
        if (i % 3 == 0) raw.back() = 0; // a final zero stays a plain byte
        packed.push_back(Pack(raw));
        Require(Unpack(packed.back(), raw.size()) == raw, "Synthetic packing round trip " + std::to_string(i));
        result.images.push_back({width, height, packed.back()});
        images_size += std::uint32_t((packed.back().size() + 3) & ~std::size_t(3));
    }
    const std::uint32_t text_offset = 0x100, text_size = 0x20;
    const std::uint32_t images_offset = text_offset + text_size;
    const std::uint32_t table_offset = images_offset + images_size;
    const std::uint32_t table_size = 16 + std::uint32_t(kDiscMessageImageCount * 16) + 8;
    Bytes& dol = result.dol;
    dol.assign(table_offset + table_size, 0);
    Put32(dol, 0x00, text_offset);
    Put32(dol, 0x48, 0x80004000u);
    Put32(dol, 0x90, text_size);
    Put32(dol, 0x1C, images_offset);
    Put32(dol, 0x48 + 0x1C, kImageBase);
    Put32(dol, 0x90 + 0x1C, images_size);
    Put32(dol, 0x20, table_offset);
    Put32(dol, 0x48 + 0x20, 0x80200000u);
    Put32(dol, 0x90 + 0x20, table_size);
    Put32(dol, 0xE0, 0x80004000u);
    std::uint32_t position = 0;
    result.table_offset = table_offset + 16;
    for (std::size_t i = 0; i < kDiscMessageImageCount; ++i)
    {
        const auto& image = result.images[i];
        std::memcpy(dol.data() + images_offset + position, image.data.data(), image.data.size());
        const std::size_t entry = result.table_offset + i * 16;
        Put32(dol, entry, image.width);
        Put32(dol, entry + 4, image.height);
        Put32(dol, entry + 8, std::uint32_t(image.data.size()));
        Put32(dol, entry + 12, kImageBase + position);
        position += std::uint32_t((image.data.size() + 3) & ~std::size_t(3));
    }
    return result;
}

std::vector<DiscMessageImage> Extract(const Bytes& dol, std::uint32_t table)
{
    return ExtractDiscMessageImages(dol.data(), dol.size(), platform::Sha1Hex(dol.data(), dol.size()), table);
}

void TestExtraction()
{
    const SyntheticDol synthetic = MakeDol();
    const auto images = Extract(synthetic.dol, synthetic.table);
    Require(images.size() == kDiscMessageImageCount, "35 images");
    for (std::size_t i = 0; i < images.size(); ++i)
        Require(images[i].width == synthetic.images[i].width && images[i].height == synthetic.images[i].height
                    && images[i].data == synthetic.images[i].data,
                "Image " + std::to_string(i) + " read back");

    RequireThrows([&] { ExtractDiscMessageImages(synthetic.dol.data(), synthetic.dol.size(), std::string(40, '0'),
                                                 synthetic.table); },
                  "SHA-1", "Another executable");
    RequireThrows([&] { ExtractDiscMessageImages(synthetic.dol.data(), 0x80, platform::Sha1Hex(synthetic.dol.data(), 0x80),
                                                 synthetic.table); },
                  "shorter than its header", "A truncated header");
    RequireThrows([&] { Extract(synthetic.dol, 0x80004000u); }, "not in a data section", "A table in a text section");
    RequireThrows([&] { Extract(synthetic.dol, synthetic.table + 16); }, "not in a data section",
                  "A table running past its section");
    RequireThrows([&] { Extract(synthetic.dol, 0x90000000u); }, "not in a data section", "A table nowhere");

    auto changed = [&](const std::function<void(Bytes&)>& change) {
        Bytes dol = synthetic.dol;
        change(dol);
        return dol;
    };
    const Bytes outside = changed([](Bytes& dol) { Put32(dol, 0x90 + 0x20, 0x100000); });
    RequireThrows([&] { Extract(outside, synthetic.table); }, "lies outside the file", "A section past the file end");
    const std::size_t entry = synthetic.table_offset + 6 * 16;
    RequireThrows([&] { Extract(changed([&](Bytes& dol) { Put32(dol, entry, 12); }), synthetic.table); },
                  "Invalid disc-message image HEADER_NoDiskImage_fre", "A width that is no multiple of 8");
    RequireThrows([&] { Extract(changed([&](Bytes& dol) { Put32(dol, entry + 4, 0); }), synthetic.table); },
                  "Invalid disc-message image", "No height");
    RequireThrows([&] { Extract(changed([&](Bytes& dol) { Put32(dol, entry + 8, 0); }), synthetic.table); },
                  "Invalid disc-message image", "No data");
    RequireThrows([&] { Extract(changed([&](Bytes& dol) { Put32(dol, entry + 12, 0x80004000u); }), synthetic.table); },
                  "is not in a data section", "Image bytes in a text section");
    RequireThrows([&] { Extract(changed([&](Bytes& dol) { Put32(dol, entry + 4, synthetic.images[6].height + 1); }),
                                synthetic.table); },
                  "unpacks to", "Image data that does not fill the picture");
}

// A stand-in for the game module's charged_disc_message_image export.
struct FakeModule
{
    std::vector<Bytes> arrays;
    std::vector<DiscMessageImage> layout;
    std::size_t count = kDiscMessageImageCount;
};
FakeModule fake;

unsigned char* FakeStorage(unsigned int index, unsigned long* size, int* width, int* height)
{
    if (index >= fake.count) return nullptr;
    *size = static_cast<unsigned long>(fake.arrays[index].size());
    *width = int(fake.layout[index].width);
    *height = int(fake.layout[index].height);
    return fake.arrays[index].data();
}

void ResetFake(const std::vector<DiscMessageImage>& images)
{
    fake = {};
    for (const auto& image : images)
    {
        fake.arrays.emplace_back(image.data.size(), 0xEE);
        fake.layout.push_back(image);
    }
    fake.count = images.size();
    // A module with extra storage past the table must still be rejected.
    fake.arrays.emplace_back(1, 0xEE);
    fake.layout.push_back({8, 1, {}});
}

bool Untouched()
{
    for (const auto& array : fake.arrays)
        for (const auto byte : array)
            if (byte != 0xEE) return false;
    return true;
}

void TestInstall()
{
    const SyntheticDol synthetic = MakeDol();
    const auto images = Extract(synthetic.dol, synthetic.table);

    ResetFake(images);
    InstallDiscMessageImages(images, FakeStorage);
    for (std::size_t i = 0; i < images.size(); ++i)
        Require(fake.arrays[i] == images[i].data, "Installed image " + std::to_string(i));

    ResetFake(images);
    fake.arrays[3].push_back(0xEE);
    RequireThrows([&] { InstallDiscMessageImages(images, FakeStorage); }, "HEADER_RetryImage_eng",
                  "A module array of another size");
    Require(Untouched(), "Nothing copied after a size mismatch");

    ResetFake(images);
    fake.layout[34].width += 8;
    RequireThrows([&] { InstallDiscMessageImages(images, FakeStorage); }, "HEADER_Loading_uke",
                  "A module picture of another width");
    Require(Untouched(), "Nothing copied after a width mismatch");

    ResetFake(images);
    fake.count = 20;
    RequireThrows([&] { InstallDiscMessageImages(images, FakeStorage); }, "no storage for disc-message image",
                  "A module with fewer images");
    Require(Untouched(), "Nothing copied for a short module");

    ResetFake(images);
    fake.count = kDiscMessageImageCount + 1;
    RequireThrows([&] { InstallDiscMessageImages(images, FakeStorage); }, "more disc-message images",
                  "A module with more images");
    Require(Untouched(), "Nothing copied for a long module");

    ResetFake(images);
    auto fewer = images;
    fewer.pop_back();
    RequireThrows([&] { InstallDiscMessageImages(fewer, FakeStorage); }, "Expected 35", "Too few images");
}

void TestNames()
{
    Require(DiscMessageImageName(0) == "HEADER_FatalErrorImage_eng", "First name");
    Require(DiscMessageImageName(4) == "HEADER_Loading_eng", "English loading");
    Require(DiscMessageImageName(12) == "HEADER_WrongDiskImage_deu", "German wrong disc");
    Require(DiscMessageImageName(34) == "HEADER_Loading_uke", "Last name");
    ++checks;
    try
    {
        DiscMessageImageName(kDiscMessageImageCount);
        throw std::runtime_error("No name past the table");
    }
    catch (const std::out_of_range&)
    {
    }
    for (std::size_t i = 0; i < ReleaseCount(); ++i)
    {
        const auto& release = ReleaseAt(i);
        Require(std::strlen(release.dol_sha1) == 40, std::string("Release hash of ") + release.game_id);
        Require(release.disc_message_table >= 0x80000000u, std::string("Release table of ") + release.game_id);
    }
}

std::uint32_t ParseHex(const char* text)
{
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 16);
    if (!end || *end) throw std::runtime_error(std::string("Not a hex number: ") + text);
    return std::uint32_t(value);
}

int ReadDisc(int argc, char** argv)
{
    // --disc FILE (--release ID REV | --sha1 HEX --table HEX)
    std::string disc, sha1, id;
    std::uint32_t table = 0;
    int revision = -1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "--disc" && i + 1 < argc) disc = argv[++i];
        else if (option == "--sha1" && i + 1 < argc) sha1 = argv[++i];
        else if (option == "--table" && i + 1 < argc) table = ParseHex(argv[++i]);
        else if (option == "--release" && i + 2 < argc)
        {
            id = argv[++i];
            revision = std::atoi(argv[++i]);
        }
        else throw std::runtime_error("Unknown argument " + option);
    }
    const Bytes dol = ReadDiscMainDol(disc);
    std::printf("main.dol %zu bytes sha1 %s\n", dol.size(), platform::Sha1Hex(dol.data(), dol.size()).c_str());
    std::vector<DiscMessageImage> images;
    if (!id.empty())
    {
        const GameRelease* release = FindRelease(id, std::uint8_t(revision));
        if (!release) throw std::runtime_error("Unknown release " + id + " revision " + std::to_string(revision));
        images = ReadDiscMessageImages(disc, *release);
    }
    else
        images = ExtractDiscMessageImages(dol.data(), dol.size(), sha1, table);
    for (std::size_t i = 0; i < images.size(); ++i)
        std::printf("%zu %s %ux%u %zu %s\n", i, DiscMessageImageName(i).c_str(), images[i].width, images[i].height,
                    images[i].data.size(), platform::Sha1Hex(images[i].data.data(), images[i].data.size()).c_str());
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1) return ReadDisc(argc, argv);
        TestSha1();
        TestNames();
        TestExtraction();
        TestInstall();
        std::printf("disc_message_images: %d checks passed\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "disc_message_images: %s\n", error.what());
        return 1;
    }
}
