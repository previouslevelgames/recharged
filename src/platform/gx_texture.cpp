#include "platform/gx_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mscharged::platform
{
namespace gx
{
namespace
{
// CMPR: tiles of 8x8 texels, four 4x4 blocks each.
void Expand565(std::uint16_t c, float out[3])
{
    out[0] = float(((c >> 11) & 31) * 255 / 31);
    out[1] = float(((c >> 5) & 63) * 255 / 63);
    out[2] = float((c & 31) * 255 / 31);
}

std::uint16_t Quantize565(const float c[3])
{
    auto q = [](float v, int max) { return std::clamp(int(std::lround(v / 255.0f * max)), 0, max); };
    return std::uint16_t((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}

// The four palette entries of a block, 0..255, and whether index 3 is transparent.
bool BlockPalette(const std::uint8_t* block, float palette[4][3])
{
    std::uint16_t c0 = BE16(block), c1 = BE16(block + 2);
    Expand565(c0, palette[0]);
    Expand565(c1, palette[1]);
    if (c0 > c1)
    {
        for (int k = 0; k < 3; ++k)
        {
            palette[2][k] = float(int(2 * palette[0][k] + palette[1][k]) / 3);
            palette[3][k] = float(int(palette[0][k] + 2 * palette[1][k]) / 3);
        }
        return false;
    }
    for (int k = 0; k < 3; ++k)
    {
        palette[2][k] = float(int(palette[0][k] + palette[1][k]) / 2);
        palette[3][k] = 0.0f;
    }
    return true;
}

int BlockIndex(const std::uint8_t* block, int x, int y) { return (block[4 + y] >> (6 - 2 * x)) & 3; }

float Distance2(const float a[3], const float b[3])
{
    float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
}

// Bit replication, as the hardware expands a component.
float Expand(unsigned value, int bits)
{
    unsigned max = (1u << bits) - 1;
    return float(value) / float(max);
}

// Texels across and down a tile, and its bytes.
bool Tile(std::uint32_t format, int& width, int& height, int& bytes)
{
    switch (format)
    {
    case kRGB5A3:
        width = height = 4;
        bytes = 32;
        return true;
    case kCMPR:
        width = height = 8;
        bytes = 32;
        return true;
    case kRGBA8:
        width = height = 4;
        bytes = 64;
        return true;
    default:
        return false;
    }
}
} // namespace

std::uint32_t BE32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint16_t BE16(const std::uint8_t* p) { return std::uint16_t((p[0] << 8) | p[1]); }
void PutBE16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = std::uint8_t(v >> 8);
    p[1] = std::uint8_t(v);
}

void PutBE32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = std::uint8_t(v >> 24);
    p[1] = std::uint8_t(v >> 16);
    p[2] = std::uint8_t(v >> 8);
    p[3] = std::uint8_t(v);
}

// The format and levels of a texture (its 32-byte header included).
bool Levels(std::span<const std::uint8_t> texture, std::uint32_t& format, HostVector<Level>& levels)
{
    if (texture.size() < 32) return false;
    format = BE32(&texture[4]);
    int tileWidth, tileHeight, tileBytes;
    if (!Tile(format, tileWidth, tileHeight, tileBytes)) return false;
    std::uint32_t count = BE32(&texture[0]);
    int width = BE16(&texture[14]), height = BE16(&texture[16]);
    if (count == 0 || count > 16 || width <= 0 || height <= 0) return false;
    std::size_t offset = 32;
    for (std::uint32_t l = 0; l < count; ++l)
    {
        std::size_t bytes = std::size_t((width + tileWidth - 1) / tileWidth)
            * std::size_t((height + tileHeight - 1) / tileHeight) * std::size_t(tileBytes);
        if (offset + bytes > texture.size()) return false;
        levels.push_back({width, height, offset});
        offset += bytes;
        width = std::max(width / 2, 1);
        height = std::max(height / 2, 1);
    }
    return true;
}

void DecodeBlock(const std::uint8_t* block, Texel out[16])
{
    float palette[4][3];
    bool transparent = BlockPalette(block, palette);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            int index = BlockIndex(block, x, y);
            out[y * 4 + x] = {{palette[index][0] / 255.0f, palette[index][1] / 255.0f, palette[index][2] / 255.0f},
                transparent && index == 3 ? 0.0f : 1.0f};
        }
}

// Encodes 16 texels (0..255) into an 8-byte block. Transparent texels need the
// three-colour mode.
void EncodeBlock(const float colours[16][3], const bool opaque[16], std::uint8_t* block)
{
    bool anyTransparent = false;
    int count = 0;
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i)
    {
        if (!opaque[i])
        {
            anyTransparent = true;
            continue;
        }
        for (int k = 0; k < 3; ++k) mean[k] += colours[i][k];
        ++count;
    }
    if (count == 0)
    {
        PutBE16(block, 0);
        PutBE16(block + 2, 0xFFFF);
        std::memset(block + 4, 0xFF, 4);
        return;
    }
    for (int k = 0; k < 3; ++k) mean[k] /= float(count);
    // Principal axis by power iteration on the covariance.
    float cov[3][3] = {};
    for (int i = 0; i < 16; ++i)
    {
        if (!opaque[i]) continue;
        float d[3] = {colours[i][0] - mean[0], colours[i][1] - mean[1], colours[i][2] - mean[2]};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) cov[a][b] += d[a] * d[b];
    }
    float axis[3] = {1.0f, 1.0f, 1.0f};
    for (int iteration = 0; iteration < 8; ++iteration)
    {
        float next[3];
        for (int a = 0; a < 3; ++a) next[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2];
        float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (length < 1e-6f) break;
        for (int a = 0; a < 3; ++a) axis[a] = next[a] / length;
    }
    float lo = std::numeric_limits<float>::max(), hi = -lo;
    for (int i = 0; i < 16; ++i)
    {
        if (!opaque[i]) continue;
        float t = (colours[i][0] - mean[0]) * axis[0] + (colours[i][1] - mean[1]) * axis[1]
            + (colours[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; ++k)
    {
        e0[k] = std::clamp(mean[k] + hi * axis[k], 0.0f, 255.0f);
        e1[k] = std::clamp(mean[k] + lo * axis[k], 0.0f, 255.0f);
    }
    std::uint16_t q0 = Quantize565(e0), q1 = Quantize565(e1);
    // Four-colour mode needs c0 > c1, three-colour mode c0 <= c1.
    if (anyTransparent ? q0 > q1 : q0 < q1) std::swap(q0, q1);
    PutBE16(block, q0);
    PutBE16(block + 2, q1);
    float palette[4][3];
    bool threeColour = BlockPalette(block, palette);
    int usable = threeColour ? 3 : 4;
    for (int y = 0; y < 4; ++y)
    {
        std::uint8_t row = 0;
        for (int x = 0; x < 4; ++x)
        {
            int i = y * 4 + x, best = 3;
            if (opaque[i])
            {
                float bestDistance = std::numeric_limits<float>::max();
                for (int p = 0; p < usable; ++p)
                {
                    float d = Distance2(colours[i], palette[p]);
                    if (d < bestDistance)
                    {
                        bestDistance = d;
                        best = p;
                    }
                }
            }
            row = std::uint8_t(row | (best << (6 - 2 * x)));
        }
        block[4 + y] = row;
    }
}

// The block holding texel (x, y).
std::size_t BlockOffset(const Level& level, int x, int y)
{
    int tilesPerRow = (level.width + 7) / 8;
    int tile = (y / 8) * tilesPerRow + x / 8;
    int sub = ((y / 4) & 1) * 2 + ((x / 4) & 1);
    return level.offset + std::size_t(tile) * 32 + std::size_t(sub) * 8;
}

// RGB5A3 and RGBA8: tiles of 4x4 texels. The offset of a texel's tile and
// its index in the tile.
std::size_t TexelTile(const Level& level, int x, int y, int tileBytes, int& index)
{
    index = (y & 3) * 4 + (x & 3);
    int tilesPerRow = (level.width + 3) / 4;
    return level.offset + std::size_t((y / 4) * tilesPerRow + x / 4) * std::size_t(tileBytes);
}

Texel DecodeRGB5A3(std::uint16_t v)
{
    if (v & 0x8000) return {{Expand((v >> 10) & 31, 5), Expand((v >> 5) & 31, 5), Expand(v & 31, 5)}, 1.0f};
    return {{Expand((v >> 8) & 15, 4), Expand((v >> 4) & 15, 4), Expand(v & 15, 4)}, Expand((v >> 12) & 7, 3)};
}

// Keeps the texel's mode and alpha bits: only its colour changes.
std::uint16_t EncodeRGB5A3(std::uint16_t original, RGB c)
{
    auto q = [](float value, int max) { return unsigned(std::clamp(int(std::lround(value * max)), 0, max)); };
    if (original & 0x8000) return std::uint16_t(0x8000 | q(c.r, 31) << 10 | q(c.g, 31) << 5 | q(c.b, 31));
    return std::uint16_t((original & 0x7000) | q(c.r, 15) << 8 | q(c.g, 15) << 4 | q(c.b, 15));
}

void DecodeLevel(std::span<const std::uint8_t> texture, std::uint32_t format, const Level& level,
    HostVector<Texel>& out)
{
    out.assign(std::size_t(level.width) * level.height, Texel{});
    if (format == kCMPR)
    {
        Texel block[16];
        for (int by = 0; by < level.height; by += 4)
            for (int bx = 0; bx < level.width; bx += 4)
            {
                DecodeBlock(&texture[BlockOffset(level, bx, by)], block);
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                        if (bx + x < level.width && by + y < level.height)
                            out[std::size_t(by + y) * level.width + bx + x] = block[y * 4 + x];
            }
        return;
    }
    for (int y = 0; y < level.height; ++y)
        for (int x = 0; x < level.width; ++x)
        {
            int index;
            Texel& t = out[std::size_t(y) * level.width + x];
            if (format == kRGB5A3)
            {
                std::size_t tile = TexelTile(level, x, y, 32, index);
                t = DecodeRGB5A3(BE16(&texture[tile + std::size_t(index) * 2]));
            }
            else
            {
                std::size_t tile = TexelTile(level, x, y, 64, index);
                const std::uint8_t* ar = &texture[tile + std::size_t(index) * 2];
                const std::uint8_t* gb = &texture[tile + 32 + std::size_t(index) * 2];
                t = {{ar[1] / 255.0f, gb[0] / 255.0f, gb[1] / 255.0f}, ar[0] / 255.0f};
            }
        }
}
} // namespace gx

using gx::BE32;

std::span<const std::uint8_t> FindBundleTexture(std::span<const std::uint8_t> bundle, std::uint32_t hash)
{
    if (bundle.size() < 16) return {};
    std::uint32_t count = BE32(&bundle[4]);
    if (count > (bundle.size() - 16) / 16) return {};
    std::size_t base = 16 + std::size_t(count) * 16;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t* entry = &bundle[16 + std::size_t(i) * 16];
        if (BE32(entry) != hash) continue;
        std::size_t offset = BE32(entry + 4), size = BE32(entry + 8);
        if (base + offset > bundle.size() || size > bundle.size() - base - offset) return {};
        return bundle.subspan(base + offset, size);
    }
    return {};
}
} // namespace mscharged::platform
