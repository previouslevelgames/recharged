#include "launcher/game_pictures.h"

#include <array>

namespace mscharged::launcher
{
namespace
{
std::uint32_t BE32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}
std::uint16_t BE16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }

constexpr std::uint32_t kRGB5A3 = 1, kCMPR = 2, kRGBA8 = 3;
constexpr std::size_t kTextureHeader = 32;
constexpr int kMostSide = 1024;

using Colour = std::array<std::uint8_t, 4>;

Colour From565(std::uint16_t c)
{
    const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    return {std::uint8_t(r << 3 | r >> 2), std::uint8_t(g << 2 | g >> 4), std::uint8_t(b << 3 | b >> 2), 255};
}

Colour FromRGB5A3(std::uint16_t c)
{
    if (c & 0x8000)
    {
        const int r = c >> 10 & 31, g = c >> 5 & 31, b = c & 31;
        return {std::uint8_t(r << 3 | r >> 2), std::uint8_t(g << 3 | g >> 2), std::uint8_t(b << 3 | b >> 2), 255};
    }
    const int a = c >> 12 & 7, r = c >> 8 & 15, g = c >> 4 & 15, b = c & 15;
    return {std::uint8_t(r * 17), std::uint8_t(g * 17), std::uint8_t(b * 17), std::uint8_t(a << 5 | a << 2 | a >> 1)};
}

void Put(Picture& out, int x, int y, const Colour& colour)
{
    if (x >= out.width || y >= out.height) return;
    const std::size_t at = (std::size_t(y) * std::size_t(out.width) + std::size_t(x)) * 4;
    for (int c = 0; c < 4; ++c) out.rgba[at + std::size_t(c)] = colour[std::size_t(c)];
}

// ByteView of the first level, by format: tiles of 8x8 (CMPR), 4x4 (RGB5A3) or
// 4x4 in two halves (RGBA8).
std::size_t LevelBytes(std::uint32_t format, int width, int height)
{
    const auto up = [](int value, int to) { return std::size_t((value + to - 1) / to * to); };
    switch (format)
    {
    case kCMPR: return up(width, 8) * up(height, 8) / 2;
    case kRGB5A3: return up(width, 4) * up(height, 4) * 2;
    case kRGBA8: return up(width, 4) * up(height, 4) * 4;
    default: return 0;
    }
}
} // namespace

std::uint32_t GameNameHash(std::string_view name)
{
    std::uint32_t hash = 0xFFFFFFFFu;
    for (const char c : name) hash += (hash << 5) + static_cast<unsigned char>(c);
    return hash;
}

ByteView BundleEntry(ByteView bundle, std::string_view name)
{
    if (!bundle.data || bundle.size < 16) return {};
    const std::uint64_t sector = BE32(bundle.data), files = BE32(bundle.data + 4);
    const std::uint64_t directory = BE32(bundle.data + 8) * sector;
    if (sector == 0 || files > bundle.size / 12 || directory > bundle.size || files * 12 > bundle.size - directory)
        return {};
    const std::uint32_t hash = GameNameHash(name);
    for (std::uint64_t n = 0; n < files; ++n)
    {
        const std::uint8_t* entry = bundle.data + directory + n * 12;
        if (BE32(entry) != hash) continue;
        const std::uint64_t at = BE32(entry + 4) * sector, length = BE32(entry + 8);
        if (at > bundle.size || length > bundle.size - at) return {};
        return {bundle.data + at, std::size_t(length)};
    }
    return {};
}

bool DecodePicture(ByteView texture, Picture& out)
{
    if (!texture.data || texture.size < kTextureHeader) return false;
    const std::uint32_t format = BE32(texture.data + 4);
    const int width = BE16(texture.data + 14), height = BE16(texture.data + 16);
    if (width <= 0 || height <= 0 || width > kMostSide || height > kMostSide) return false;
    const std::size_t bytes = LevelBytes(format, width, height);
    if (!bytes || bytes > texture.size - kTextureHeader) return false;
    out.width = width;
    out.height = height;
    out.rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);
    const std::uint8_t* p = texture.data + kTextureHeader;
    if (format == kCMPR)
    {
        for (int ty = 0; ty < height; ty += 8)
            for (int tx = 0; tx < width; tx += 8)
                for (int block = 0; block < 4; ++block, p += 8)
                {
                    const std::uint16_t c0 = BE16(p), c1 = BE16(p + 2);
                    Colour palette[4] = {From565(c0), From565(c1), {}, {}};
                    for (int c = 0; c < 3; ++c)
                    {
                        const int a = palette[0][std::size_t(c)], b = palette[1][std::size_t(c)];
                        palette[2][std::size_t(c)] = std::uint8_t(c0 > c1 ? (2 * a + b) / 3 : (a + b) / 2);
                        palette[3][std::size_t(c)] = std::uint8_t(c0 > c1 ? (a + 2 * b) / 3 : 0);
                    }
                    palette[2][3] = 255;
                    palette[3][3] = c0 > c1 ? 255 : 0;
                    const int bx = tx + (block & 1) * 4, by = ty + (block >> 1) * 4;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x)
                            Put(out, bx + x, by + y, palette[(p[4 + y] >> (6 - 2 * x)) & 3]);
                }
    }
    else if (format == kRGB5A3)
    {
        for (int ty = 0; ty < height; ty += 4)
            for (int tx = 0; tx < width; tx += 4)
                for (int n = 0; n < 16; ++n, p += 2) Put(out, tx + n % 4, ty + n / 4, FromRGB5A3(BE16(p)));
    }
    else
    {
        // RGBA8: a tile's 16 AR pairs, then its 16 GB pairs.
        for (int ty = 0; ty < height; ty += 4)
            for (int tx = 0; tx < width; tx += 4, p += 64)
                for (int n = 0; n < 16; ++n)
                    Put(out, tx + n % 4, ty + n / 4,
                        {p[2 * n + 1], p[32 + 2 * n], p[32 + 2 * n + 1], p[2 * n]});
    }
    return true;
}

const char* CaptainName(int team)
{
    // eTeamID order; the names are the game's CharacterInfo names.
    static constexpr const char* kNames[] = {"mario",  "bowser",  "daisy", "donkeykong", "luigi",     "peach",
                                             "waluigi", "wario",  "yoshi", "bowserjr",   "diddykong", "petey"};
    return team >= 0 && team < int(std::size(kNames)) ? kNames[team] : nullptr;
}

const char* SidekickName(int sidekick)
{
    // eSidekickID order.
    static constexpr const char* kNames[] = {"toad", "koopa",    "hammerbro",  "birdo",
                                             "boo",  "drybones", "montymole", "shyguy"};
    return sidekick >= 0 && sidekick < int(std::size(kNames)) ? kNames[sidekick] : nullptr;
}

std::string CaptainIconPath(int team)
{
    const char* captain = CaptainName(team);
    return captain ? std::string("fe/captain_icons/captain_icons_") + captain : std::string();
}

std::string SidekickPicturePath(int sidekick, int team, bool alt)
{
    const char* name = SidekickName(sidekick);
    const char* captain = CaptainName(team);
    if (!name || !captain) return {};
    return std::string("fe/sidekick_images/attributes_") + name + "_" + captain + (alt ? "_alt" : "");
}
} // namespace mscharged::launcher
