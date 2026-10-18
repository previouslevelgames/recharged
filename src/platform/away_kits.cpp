#include "platform/away_kits.h"

#include "platform/gx_texture.h"
#include "platform/kit_colour.h"
#include "platform/recoloured_texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace mscharged::platform
{
using namespace gx;

namespace
{
// A hue band in HSV: hue in degrees (lo > hi wraps around 0), minimum
// saturation (for light and for dark texels: pale anti-aliased edges are kit,
// dull dark straps are not) and value.
struct Band
{
    float hueLo, hueHi, minSaturation, minDarkSaturation, minValue;
};
} // namespace

struct AwayKit
{
    const char* captain;
    std::uint32_t source;      // the kit texture (diffuse of the captain's model), 0 if the game has the kit
    std::uint32_t colour;      // the away colour, 0xRRGGBB
    Band strong;               // clearly the kit's colour
    Band weak;                 // its shading and highlights, when connected to strong texels
    const char* references[2]; // teams compared with on team textures
    bool photos;               // the game's photos of him recolour cleanly (their backgrounds lack his colour)
};

namespace
{
// Contrasting kits none of the game's teams wears at home, after each
// character's best-known colours where he has some: Mario's blue, after his
// overalls and the away side's blue in the menus.
// Luigi's, Wario's, Waluigi's and Peach's newspaper photos have grass, crowds
// or stands in their colours behind them.
const AwayKit kKits[] = {
    {"mario", 0xBBFFCF5Eu, 0x2B5FE3u, {345.0f, 12.0f, 0.55f, 0.55f, 0.25f}, {335.0f, 10.0f, 0.22f, 0.25f, 0.04f},
        {"luigi", "waluigi"}, true},
    {"luigi", 0x4442532Du, 0x3D9BE9u, {95.0f, 150.0f, 0.45f, 0.45f, 0.15f}, {80.0f, 165.0f, 0.15f, 0.15f, 0.05f},
        {"mario", "wario"}, false},
    {"wario", 0x31A701C7u, 0x2E5CD8u, {40.0f, 58.0f, 0.55f, 0.55f, 0.30f}, {36.0f, 64.0f, 0.30f, 0.30f, 0.10f},
        {"luigi", "waluigi"}, false},
    {"waluigi", 0xD04A32DFu, 0xF08A24u, {255.0f, 290.0f, 0.45f, 0.45f, 0.18f}, {228.0f, 315.0f, 0.15f, 0.15f, 0.05f},
        {"luigi", "wario"}, false},
    // Peach's blue kit is on the disc (peach_alt.rlt), her menu pictures and
    // Mega Strike hand in it are not: those are made in its colour.
    {"peach", 0u, 0x5DB9FFu, {312.0f, 338.0f, 0.30f, 0.30f, 0.30f}, {300.0f, 341.0f, 0.10f, 0.18f, 0.10f},
        {"luigi", "wario"}, false},
};

using kit_colour::Lab;
using kit_colour::LchStats;
using kit_colour::ToLab;
using kit_colour::ToHsv;

bool InBand(const Band& band, float h, float s, float v)
{
    bool hue = band.hueLo <= band.hueHi ? (h >= band.hueLo && h <= band.hueHi) : (h >= band.hueLo || h <= band.hueHi);
    return hue && s >= (v >= 0.45f ? band.minSaturation : band.minDarkSaturation) && v >= band.minValue;
}

enum : std::uint8_t
{
    kNone = 0,
    kKit = 1, // recoloured as the away kit
};

// Kit texels of a kit texture or picture: strong texels and the weak ones
// connected to them.
void KitMask(const AwayKit& kit, const Level& level, const HostVector<Texel>& texels, HostVector<std::uint8_t>& mask)
{
    std::size_t count = texels.size();
    mask.assign(count, kNone);
    HostVector<std::uint8_t> weak(count, 0);
    HostVector<int> queue;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (texels[i].alpha <= 0.0f) continue;
        float h, s, v;
        ToHsv(texels[i].colour, h, s, v);
        if (InBand(kit.strong, h, s, v))
        {
            mask[i] = kKit;
            queue.push_back(int(i));
        }
        else if (InBand(kit.weak, h, s, v))
            weak[i] = 1;
    }
    for (std::size_t q = 0; q < queue.size(); ++q)
    {
        int i = queue[q], x = i % level.width, y = i / level.width;
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; ++k)
        {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= level.width || ny[k] >= level.height) continue;
            int j = ny[k] * level.width + nx[k];
            if (weak[j] && mask[j] == kNone)
            {
                mask[j] = kKit;
                queue.push_back(j);
            }
        }
    }
}

// A painting's dark kit shading is only a few bits deep: its hue jumps around
// and some texels drop out of the kit's bands, leaving specks of the home
// colour under a cap's brim. A dark texel mostly surrounded by kit texels
// belongs to the kit.
void FillDarkHoles(const Level& level, const HostVector<Texel>& texels, HostVector<std::uint8_t>& mask)
{
    const int w = level.width, h = level.height;
    HostVector<std::uint8_t> next;
    for (int pass = 0; pass < 2; ++pass)
    {
        next = mask;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                std::size_t i = std::size_t(y) * w + x;
                if (mask[i] == kKit || texels[i].alpha <= 0.0f) continue;
                float hue, s, v;
                ToHsv(texels[i].colour, hue, s, v);
                if (v >= 0.3f) continue;
                int kit = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        int nx = x + dx, ny = y + dy;
                        if ((dx != 0 || dy != 0) && nx >= 0 && ny >= 0 && nx < w && ny < h
                            && mask[std::size_t(ny) * w + nx] == kKit)
                            ++kit;
                    }
                if (kit >= 5) next[i] = kKit;
            }
        mask.swap(next);
    }
}

// Pictures are paintings: single texels and thin lines of hair and skin
// shading have the kit's hue as well, kits are areas. Keeps the kit texels next
// to one whose 3x3 neighbourhood is all kit (a morphological opening).
void OpenMask(const Level& level, HostVector<std::uint8_t>& mask)
{
    const int w = level.width, h = level.height;
    HostVector<std::uint8_t> core(mask.size(), 0);
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x)
        {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; ++dy)
                for (int dx = -1; dx <= 1 && all; ++dx) all = mask[std::size_t(y + dy) * w + x + dx] == kKit;
            core[std::size_t(y) * w + x] = all;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            std::uint8_t& m = mask[std::size_t(y) * w + x];
            bool near = false;
            for (int dy = -1; dy <= 1 && m == kKit && !near; ++dy)
                for (int dx = -1; dx <= 1 && !near; ++dx)
                {
                    int nx = x + dx, ny = y + dy;
                    near = nx >= 0 && ny >= 0 && nx < w && ny < h && core[std::size_t(ny) * w + nx];
                }
            if (!near) m = kNone;
        }
}

// Team texels of a team texture: those that differ from both references, as
// only the team colour does, and have the kit's hue (where the other teams'
// emblems differ in shape, this texture is white or grey).
void TeamMask(const AwayKit& kit, const HostVector<Texel>& texels, const HostVector<Texel>& reference0,
    const HostVector<Texel>& reference1, HostVector<std::uint8_t>& mask)
{
    constexpr float kThreshold = 0.07f * 0.07f;
    const Band hue = {kit.weak.hueLo - 10.0f < 0.0f ? kit.weak.hueLo + 350.0f : kit.weak.hueLo - 10.0f,
        kit.weak.hueHi + 10.0f >= 360.0f ? kit.weak.hueHi - 350.0f : kit.weak.hueHi + 10.0f, 0.12f, 0.12f, 0.04f};
    mask.assign(texels.size(), kNone);
    for (std::size_t i = 0; i < texels.size(); ++i)
    {
        if (texels[i].alpha <= 0.0f) continue;
        float h, s, v;
        ToHsv(texels[i].colour, h, s, v);
        if (!InBand(hue, h, s, v)) continue;
        Lab c = ToLab(texels[i].colour), r0 = ToLab(reference0[i].colour), r1 = ToLab(reference1[i].colour);
        auto d2 = [](Lab a, Lab b) {
            float dl = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
            return dl * dl + da * da + db * db;
        };
        if (d2(c, r0) > kThreshold && d2(c, r1) > kThreshold) mask[i] = kKit;
    }
}

LchStats Median(const HostVector<Texel>& texels, const HostVector<std::uint8_t>& mask)
{
    HostVector<float> L, C, a, b;
    for (std::size_t i = 0; i < texels.size(); ++i)
    {
        if (mask[i] != kKit) continue;
        Lab c = ToLab(texels[i].colour);
        L.push_back(c.L);
        C.push_back(std::sqrt(c.a * c.a + c.b * c.b));
        a.push_back(c.a);
        b.push_back(c.b);
    }
    if (L.empty()) return {0.5f, 0.1f, 0.0f};
    auto median = [](HostVector<float>& v) {
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    float ma = median(a), mb = median(b);
    return {median(L), median(C), std::atan2(mb, ma)};
}

RGB Recolour(RGB colour, const LchStats& from, const LchStats& to) { return kit_colour::Recolour(colour, from, to); }

// The host's HD recolour of texture packs (charged_configure_hd_recolour), or null.
ChargedRecolouredTextureHook sRecolouredHook = nullptr;

// The bytes of one level in its format (CMPR 8x8 tiles of 4 bits per texel,
// RGB5A3 4x4 tiles of 16 bits, RGBA8 4x4 tiles of 32 bits).
std::size_t LevelBytes(std::uint32_t format, const Level& level)
{
    const std::size_t tiles = format == kCMPR ? std::size_t((level.width + 7) / 8) * ((level.height + 7) / 8)
                                              : std::size_t((level.width + 3) / 4) * ((level.height + 3) / 4);
    return tiles * (format == kCMPR ? 32 : format == kRGB5A3 ? 32 : 64);
}

std::uint8_t Byte(float value) { return std::uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); }

bool DecodeReference(std::span<const std::uint8_t> reference, std::uint32_t format, const Level& level,
    std::size_t levelIndex, HostVector<Texel>& out)
{
    HostVector<Level> levels;
    std::uint32_t referenceFormat;
    if (!Levels(reference, referenceFormat, levels) || referenceFormat != format || levels.size() <= levelIndex)
        return false;
    const Level& other = levels[levelIndex];
    if (other.width != level.width || other.height != level.height) return false;
    DecodeLevel(reference, format, other, out);
    return true;
}

// Writes the recoloured texels of a level back in its format. CMPR blocks
// with a kit texel are encoded anew; other blocks and texels keep their bytes.
void WriteLevel(std::span<std::uint8_t> texture, std::uint32_t format, const Level& level,
    const HostVector<std::uint8_t>& mask, const LchStats& from, const LchStats& to)
{
    if (format == kCMPR)
    {
        for (int by = 0; by < level.height; by += 4)
            for (int bx = 0; bx < level.width; bx += 4)
            {
                std::uint8_t* block = &texture[BlockOffset(level, bx, by)];
                bool touched = false;
                float colours[16][3];
                bool opaque[16];
                Texel decoded[16];
                DecodeBlock(block, decoded);
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                    {
                        int i = y * 4 + x;
                        RGB c = decoded[i].colour;
                        opaque[i] = decoded[i].alpha > 0.0f;
                        if (bx + x < level.width && by + y < level.height
                            && mask[std::size_t(by + y) * level.width + bx + x] == kKit)
                        {
                            c = Recolour(c, from, to);
                            touched = true;
                        }
                        colours[i][0] = c.r * 255.0f;
                        colours[i][1] = c.g * 255.0f;
                        colours[i][2] = c.b * 255.0f;
                    }
                if (touched) EncodeBlock(colours, opaque, block);
            }
        return;
    }
    for (int y = 0; y < level.height; ++y)
        for (int x = 0; x < level.width; ++x)
        {
            if (mask[std::size_t(y) * level.width + x] != kKit) continue;
            int index;
            if (format == kRGB5A3)
            {
                std::uint8_t* p = &texture[TexelTile(level, x, y, 32, index) + std::size_t(index) * 2];
                std::uint16_t v = BE16(p);
                PutBE16(p, EncodeRGB5A3(v, Recolour(DecodeRGB5A3(v).colour, from, to)));
            }
            else
            {
                std::size_t tile = TexelTile(level, x, y, 64, index);
                std::uint8_t* ar = &texture[tile + std::size_t(index) * 2];
                std::uint8_t* gb = &texture[tile + 32 + std::size_t(index) * 2];
                RGB c = Recolour({ar[1] / 255.0f, gb[0] / 255.0f, gb[1] / 255.0f}, from, to);
                ar[1] = std::uint8_t(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255.0f));
                gb[0] = std::uint8_t(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255.0f));
                gb[1] = std::uint8_t(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255.0f));
            }
        }
}
} // namespace

const AwayKit* FindGeneratedAwayKit(const char* captain)
{
    if (!captain) return nullptr;
    for (const AwayKit& kit : kKits)
        if (std::strcmp(kit.captain, captain) == 0) return &kit;
    return nullptr;
}

std::uint32_t AwayKitColour(const AwayKit& kit) { return kit.colour; }
std::uint32_t AwayKitSourceTexture(const AwayKit& kit) { return kit.source; }
const char* AwayKitReference(const AwayKit& kit, int index) { return kit.references[index & 1]; }
bool AwayKitPhotos(const AwayKit& kit) { return kit.photos; }

bool RecolourAwayKit(const AwayKit& kit, AwayKitTexture kind, std::span<std::uint8_t> texture,
    std::span<const std::uint8_t> reference0, std::span<const std::uint8_t> reference1)
{
    HostVector<Level> levels;
    std::uint32_t format;
    if (!Levels(texture, format, levels)) return false;
    HostVector<Texel> texels, ref0, ref1;
    HostVector<std::uint8_t> mask;
    const LchStats target = kit_colour::Target<RGB>(kit.colour);
    LchStats from{};
    for (std::size_t l = 0; l < levels.size(); ++l)
    {
        const Level& level = levels[l];
        DecodeLevel(texture, format, level, texels);
        if (kind != AwayKitTexture::Team)
        {
            KitMask(kit, level, texels, mask);
            if (kind == AwayKitTexture::Picture) FillDarkHoles(level, texels, mask);
            // Small mip levels have no room for the opening's 3x3 cores.
            if (kind == AwayKitTexture::Picture && level.width >= 16 && level.height >= 16) OpenMask(level, mask);
        }
        else
        {
            if (!DecodeReference(reference0, format, level, l, ref0)
                || !DecodeReference(reference1, format, level, l, ref1))
                return false;
            TeamMask(kit, texels, ref0, ref1, mask);
        }
        // The reference colours come from the full-size level, so every mip
        // level gets the same mapping.
        if (l == 0) from = Median(texels, mask);
        // Texture packs: the host recolours the pack's HD image of the largest
        // level alike, from its texels before and the texels recoloured.
        HostVector<std::uint8_t> original, rgba;
        const std::size_t bytes = LevelBytes(format, level);
        const bool tell = l == 0 && sRecolouredHook && level.offset + bytes <= texture.size()
            && std::find(mask.begin(), mask.end(), std::uint8_t(kKit)) != mask.end();
        if (tell)
        {
            original.assign(texture.begin() + std::ptrdiff_t(level.offset),
                texture.begin() + std::ptrdiff_t(level.offset + bytes));
            rgba.resize(texels.size() * 4);
            for (std::size_t i = 0; i < texels.size(); ++i)
            {
                rgba[i * 4] = Byte(texels[i].colour.r);
                rgba[i * 4 + 1] = Byte(texels[i].colour.g);
                rgba[i * 4 + 2] = Byte(texels[i].colour.b);
                rgba[i * 4 + 3] = Byte(texels[i].alpha);
            }
        }
        WriteLevel(texture, format, level, mask, from, target);
        if (tell)
        {
            const ChargedRecolouredTexture info{format == kCMPR ? 14u : format == kRGB5A3 ? 5u : 6u,
                unsigned(level.width), unsigned(level.height), unsigned(levels.size()), original.data(),
                texture.data() + level.offset, (unsigned long)bytes, mask.data(), rgba.data(),
                {target.L, target.C, target.h}, int(kind)};
            sRecolouredHook(&info);
        }
    }
    return true;
}

}

// The tweak hooks' interface (include/mscharged_tweaks.h of the tweak series).
using namespace mscharged::platform;

extern "C" {
unsigned mscharged_tweak_away_kit_colour(const char* captain)
{
    const AwayKit* kit = FindGeneratedAwayKit(captain);
    return kit ? AwayKitColour(*kit) : 0;
}

unsigned mscharged_tweak_away_kit_source(const char* captain)
{
    const AwayKit* kit = FindGeneratedAwayKit(captain);
    return kit ? AwayKitSourceTexture(*kit) : 0;
}

const char* mscharged_tweak_away_kit_reference(const char* captain, int index)
{
    const AwayKit* kit = FindGeneratedAwayKit(captain);
    return kit ? AwayKitReference(*kit, index) : nullptr;
}

bool mscharged_tweak_away_kit_photos(const char* captain)
{
    const AwayKit* kit = FindGeneratedAwayKit(captain);
    return kit && AwayKitPhotos(*kit);
}

// Host entry: the HD recolour of texture packs, told about each recoloured texture.
__attribute__((visibility("default"))) void charged_configure_hd_recolour(ChargedRecolouredTextureHook hook)
{
    sRecolouredHook = hook;
}

// The kinds of mscharged_tweaks.h (0 kit texture, 1 team texture, 2 picture).
bool mscharged_tweak_recolour_away_texture(const char* captain, int kind, void* texture, unsigned long size,
    const void* reference0, unsigned long size0, const void* reference1, unsigned long size1)
{
    const AwayKit* kit = FindGeneratedAwayKit(captain);
    bool team = kind == 1;
    if (!kit || !texture || kind < 0 || kind > 2 || (team && (!reference0 || !reference1))) return false;
    std::span<const std::uint8_t> r0, r1;
    if (team)
    {
        r0 = {static_cast<const std::uint8_t*>(reference0), size0};
        r1 = {static_cast<const std::uint8_t*>(reference1), size1};
    }
    return RecolourAwayKit(*kit, kind == 0 ? AwayKitTexture::Kit : team ? AwayKitTexture::Team : AwayKitTexture::Picture,
        {static_cast<std::uint8_t*>(texture), size}, r0, r1);
}

bool mscharged_tweak_make_away_kit(const char* captain, bool team, void* bundle, unsigned long size, unsigned source,
    unsigned target, const void* reference0, unsigned long size0, unsigned texture0, const void* reference1,
    unsigned long size1, unsigned texture1)
{
    const AwayKit* kit = FindGeneratedAwayKit(captain);
    if (!kit || !bundle) return false;
    std::span<std::uint8_t> bytes(static_cast<std::uint8_t*>(bundle), size);
    std::span<const std::uint8_t> texture = FindBundleTexture(bytes, source);
    if (texture.empty()) return false;
    std::span<std::uint8_t> writable(const_cast<std::uint8_t*>(texture.data()), texture.size());
    std::span<const std::uint8_t> r0, r1;
    if (team)
    {
        if (!reference0 || !reference1) return false;
        r0 = FindBundleTexture({static_cast<const std::uint8_t*>(reference0), size0}, texture0);
        r1 = FindBundleTexture({static_cast<const std::uint8_t*>(reference1), size1}, texture1);
    }
    bool recoloured = RecolourAwayKit(*kit, team ? AwayKitTexture::Team : AwayKitTexture::Kit, writable, r0, r1);
    // Rename it, recoloured or not: the game registers it beside the home kit,
    // and the away team's characters always find their texture.
    std::uint32_t count = (std::uint32_t(bytes[4]) << 24) | (std::uint32_t(bytes[5]) << 16) | (std::uint32_t(bytes[6]) << 8) | bytes[7];
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint8_t* entry = &bytes[16 + std::size_t(i) * 16];
        std::uint32_t hash = (std::uint32_t(entry[0]) << 24) | (std::uint32_t(entry[1]) << 16) | (std::uint32_t(entry[2]) << 8) | entry[3];
        if (hash != source) continue;
        PutBE32(entry, target);
        return recoloured;
    }
    return false;
}
}
