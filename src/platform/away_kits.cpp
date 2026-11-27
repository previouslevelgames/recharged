#include "platform/away_kits.h"

#include "platform/gx_texture.h"
#include "platform/kit_colour.h"
#include "platform/recoloured_texture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <unordered_map>
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
    kKit = 1,  // recoloured as the away kit
    kTrim = 2, // a team texture's second colour, recoloured as the trim (unfinished kits)
};

// The team colours of an unfinished kit's team texture: the band of the team
// colour, and of its second colour (the trim) with the colour the trim becomes.
struct TeamColours
{
    Band team;
    Band trim;
    std::uint32_t trimColour; // 0xRRGGBB
};

// What the masks need of a texel's colour.
struct ColourInfo
{
    float h, s, v; // HSV, hue in degrees
    Lab lab;       // OKLab
};

ColourInfo InfoOf(RGB c)
{
    ColourInfo info;
    ToHsv(c, info.h, info.s, info.v);
    info.lab = ToLab(c);
    return info;
}

// CMPR textures repeat few colours (Shy Guy's 256x256 kit and its mip levels:
// some 3,500 in 87,000 texels), so each colour's conversions are worked out
// once. Keyed by the colour's exact bits: the results are the same.
template<class Value> class ColourMemo
{
public:
    template<class Compute> Value Get(RGB c, Compute compute)
    {
        const Key key{std::bit_cast<std::uint32_t>(c.r), std::bit_cast<std::uint32_t>(c.g),
            std::bit_cast<std::uint32_t>(c.b)};
        auto [entry, added] = values_.try_emplace(key);
        if (added) entry->second = compute(c);
        return entry->second;
    }

private:
    struct Key
    {
        std::uint32_t r, g, b;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash
    {
        std::size_t operator()(const Key& key) const noexcept
        {
            std::uint64_t h = key.r * 0x9E3779B97F4A7C15ull;
            h = (h ^ key.g) * 0xC2B2AE3D27D4EB4Full;
            h = (h ^ key.b) * 0x165667B19E3779F9ull;
            return std::size_t(h ^ (h >> 32));
        }
    };
    std::unordered_map<Key, Value, KeyHash, std::equal_to<Key>, HostAllocator<std::pair<const Key, Value>>> values_;
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

// The colours a team texture's team texels have: the kit's weak band and a
// little more.
Band TeamBand(const AwayKit& kit)
{
    return {kit.weak.hueLo - 10.0f < 0.0f ? kit.weak.hueLo + 350.0f : kit.weak.hueLo - 10.0f,
        kit.weak.hueHi + 10.0f >= 360.0f ? kit.weak.hueHi - 350.0f : kit.weak.hueHi + 10.0f, 0.12f, 0.12f, 0.04f};
}

// Team texels of a team texture: those that differ from both references, as
// only the team colours do (where the other teams' emblems differ in shape,
// this texture is white or grey), and have the team colour (`team`: kKit) or
// the trim's (`trim`, if any: kTrim).
void TeamMask(const Band& team, const Band* trim, const HostVector<Texel>& texels, const HostVector<Texel>& reference0,
    const HostVector<Texel>& reference1, ColourMemo<ColourInfo>& colours, HostVector<std::uint8_t>& mask)
{
    constexpr float kThreshold = 0.07f * 0.07f;
    auto d2 = [](Lab a, Lab b) {
        float dl = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
        return dl * dl + da * da + db * db;
    };
    mask.assign(texels.size(), kNone);
    for (std::size_t i = 0; i < texels.size(); ++i)
    {
        if (texels[i].alpha <= 0.0f) continue;
        const ColourInfo c = colours.Get(texels[i].colour, InfoOf);
        std::uint8_t value;
        if (InBand(team, c.h, c.s, c.v))
            value = kKit;
        else if (trim && InBand(*trim, c.h, c.s, c.v))
            value = kTrim;
        else
            continue;
        const Lab r0 = colours.Get(reference0[i].colour, InfoOf).lab;
        const Lab r1 = colours.Get(reference1[i].colour, InfoOf).lab;
        if (d2(c.lab, r0) > kThreshold && d2(c.lab, r1) > kThreshold) mask[i] = value;
    }
}

// The median colour of the texels of one mask value.
LchStats Median(const HostVector<Texel>& texels, const HostVector<std::uint8_t>& mask, std::uint8_t value,
    ColourMemo<ColourInfo>& colours)
{
    HostVector<float> L, C, a, b;
    for (std::size_t i = 0; i < texels.size(); ++i)
    {
        if (mask[i] != value) continue;
        const Lab c = colours.Get(texels[i].colour, InfoOf).lab;
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

// How the texels of one mask value recolour: their median colour `from`
// becomes `to`. Set both before the first colour.
struct Mapping
{
    LchStats from{}, to{};
    ColourMemo<RGB> recoloured;

    RGB operator()(RGB c)
    {
        return recoloured.Get(c, [this](RGB colour) { return Recolour(colour, from, to); });
    }
};

// Writes the recoloured texels of a level back in its format, each by the
// mapping of its mask value (kKit, kTrim). CMPR blocks with a recoloured texel
// are encoded anew; other blocks and texels keep their bytes.
void WriteLevel(std::span<std::uint8_t> texture, std::uint32_t format, const Level& level,
    const HostVector<std::uint8_t>& mask, Mapping (&maps)[3])
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
                        const std::uint8_t m = bx + x < level.width && by + y < level.height
                            ? mask[std::size_t(by + y) * level.width + bx + x]
                            : std::uint8_t(kNone);
                        if (m != kNone)
                        {
                            c = maps[m](c);
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
            const std::uint8_t m = mask[std::size_t(y) * level.width + x];
            if (m == kNone) continue;
            int index;
            if (format == kRGB5A3)
            {
                std::uint8_t* p = &texture[TexelTile(level, x, y, 32, index) + std::size_t(index) * 2];
                std::uint16_t v = BE16(p);
                PutBE16(p, EncodeRGB5A3(v, maps[m](DecodeRGB5A3(v).colour)));
            }
            else
            {
                std::size_t tile = TexelTile(level, x, y, 64, index);
                std::uint8_t* ar = &texture[tile + std::size_t(index) * 2];
                std::uint8_t* gb = &texture[tile + 32 + std::size_t(index) * 2];
                RGB c = maps[m]({ar[1] / 255.0f, gb[0] / 255.0f, gb[1] / 255.0f});
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

namespace
{
// RecolourAwayKit; on a team texture `colours`, if given, replace the kit's
// team colour band and add a trim (an unfinished kit).
bool RecolourTexture(const AwayKit& kit, AwayKitTexture kind, std::span<std::uint8_t> texture,
    std::span<const std::uint8_t> reference0, std::span<const std::uint8_t> reference1, const TeamColours* colours)
{
    if (colours && kind != AwayKitTexture::Team) return false;
    HostVector<Level> levels;
    std::uint32_t format;
    if (!Levels(texture, format, levels)) return false;
    HostVector<Texel> texels, ref0, ref1;
    HostVector<std::uint8_t> mask;
    ColourMemo<ColourInfo> infos;
    const LchStats target = kit_colour::Target<RGB>(kit.colour);
    Mapping maps[3];
    maps[kKit].to = target;
    if (colours) maps[kTrim].to = kit_colour::Target<RGB>(colours->trimColour);
    const Band team = colours ? colours->team : TeamBand(kit);
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
            TeamMask(team, colours ? &colours->trim : nullptr, texels, ref0, ref1, infos, mask);
        }
        // The reference colours come from the full-size level, so every mip
        // level gets the same mapping.
        if (l == 0)
        {
            maps[kKit].from = Median(texels, mask, kKit, infos);
            if (colours) maps[kTrim].from = Median(texels, mask, kTrim, infos);
        }
        // Texture packs: the host recolours the pack's HD image of the largest
        // level alike, from its texels before and the texels recoloured. It
        // knows one colour: a texture with a trim keeps the game's resolution.
        HostVector<std::uint8_t> original, rgba;
        const std::size_t bytes = LevelBytes(format, level);
        const bool tell = l == 0 && sRecolouredHook && !colours && level.offset + bytes <= texture.size()
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
        WriteLevel(texture, format, level, mask, maps);
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
} // namespace

bool RecolourAwayKit(const AwayKit& kit, AwayKitTexture kind, std::span<std::uint8_t> texture,
    std::span<const std::uint8_t> reference0, std::span<const std::uint8_t> reference1)
{
    return RecolourTexture(kit, kind, texture, reference0, reference1, nullptr);
}

struct UnfinishedKit
{
    std::uint32_t texture;     // its name (the game's string hash)
    std::uint64_t disc;        // FNV-1a of its bytes on the disc (header and levels)
    const char* captain;       // whose away kit gives the team colour (FindGeneratedAwayKit)
    TeamColours colours;       // its team colours
    const char* files[3];      // UnfinishedKitFile
    std::uint32_t textures[3]; // the textures in them
};

namespace
{
// Identical on the US, European (both revisions), Japanese and Korean discs.
// Peach's other sidekicks' blue kits turn her pink (#E2578B, the median) into
// #5CB8FE, her away colour 0x5DB9FF, and her light blue (#67B6F9) into
// #FFF422. Shy Guy's pink reaches into red where it shades into black lines.
const UnfinishedKit kUnfinished[] = {
    {0xD88042C0u /* shyguy_peach_alt/shyguy_peach_alt */, 0x35E3DB13379D0F65ull, "peach",
        {{285.0f, 20.0f, 0.12f, 0.12f, 0.04f}, {185.0f, 235.0f, 0.15f, 0.15f, 0.04f}, 0xFFF422u},
        {"art/characters/shyguy/shyguy_peach.rlt", "art/characters/shyguy/shyguy_luigi.rlt",
            "art/characters/shyguy/shyguy_wario.rlt"},
        {0xD4E5D8C0u /* shyguy_peach/shyguy_peach */, 0x73F31692u /* shyguy_luigi/shyguy_luigi */,
            0x4E8BACA2u /* shyguy_wario/shyguy_wario */}},
};

std::uint64_t Fnv1a(std::span<const std::uint8_t> bytes)
{
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::uint8_t b : bytes) hash = (hash ^ b) * 0x100000001B3ull;
    return hash;
}
} // namespace

const UnfinishedKit* FindUnfinishedKit(std::span<const std::uint8_t> bundle)
{
    for (const UnfinishedKit& kit : kUnfinished)
    {
        std::span<const std::uint8_t> texture = FindBundleTexture(bundle, kit.texture);
        if (!texture.empty() && Fnv1a(texture) == kit.disc) return &kit;
    }
    return nullptr;
}

const UnfinishedKit* UnfinishedKitNamed(std::uint32_t texture)
{
    for (const UnfinishedKit& kit : kUnfinished)
        if (kit.texture == texture) return &kit;
    return nullptr;
}

const char* UnfinishedKitFile(const UnfinishedKit& kit, int index)
{
    return index >= 0 && index < 3 ? kit.files[index] : nullptr;
}

bool FinishUnfinishedKit(const UnfinishedKit& kit, std::span<std::uint8_t> bundle, std::span<const std::uint8_t> source,
    std::span<const std::uint8_t> reference0, std::span<const std::uint8_t> reference1)
{
    const AwayKit* away = FindGeneratedAwayKit(kit.captain);
    std::span<const std::uint8_t> target = FindBundleTexture(bundle, kit.texture);
    std::span<const std::uint8_t> from = FindBundleTexture(source, kit.textures[0]);
    if (!away || target.empty() || from.size() != target.size() || from.size() < 32
        || std::memcmp(from.data(), target.data(), 32) != 0)
        return false;
    HostVector<std::uint8_t> made(from.begin(), from.end());
    if (!RecolourTexture(*away, AwayKitTexture::Team, made, FindBundleTexture(reference0, kit.textures[1]),
            FindBundleTexture(reference1, kit.textures[2]), &kit.colours))
        return false;
    std::memcpy(const_cast<std::uint8_t*>(target.data()), made.data(), made.size());
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

// Unfinished kits (issue #176): whether a loaded sidekick bundle holds one as
// the disc has it, with the files it is made from (UnfinishedKitFile); and
// making it in the bundle from those files' bundles.
bool mscharged_tweak_unfinished_kit(const void* bundle, unsigned long size, const char* files[3])
{
    const UnfinishedKit* kit = bundle ? FindUnfinishedKit({static_cast<const std::uint8_t*>(bundle), size}) : nullptr;
    for (int i = 0; i < 3; ++i) files[i] = kit ? UnfinishedKitFile(*kit, i) : nullptr;
    return kit != nullptr;
}

bool mscharged_tweak_finish_unfinished_kit(void* bundle, unsigned long size, const void* source,
    unsigned long source_size, const void* reference0, unsigned long size0, const void* reference1, unsigned long size1)
{
    if (!bundle || !source || !reference0 || !reference1) return false;
    std::span<std::uint8_t> bytes(static_cast<std::uint8_t*>(bundle), size);
    const UnfinishedKit* kit = FindUnfinishedKit(bytes);
    auto in = [](const void* data, unsigned long length) {
        return std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data), length);
    };
    return kit && FinishUnfinishedKit(*kit, bytes, in(source, source_size), in(reference0, size0), in(reference1, size1));
}
}
