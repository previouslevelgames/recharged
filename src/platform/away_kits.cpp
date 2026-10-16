#include "platform/away_kits.h"

#include "platform/host_metadata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace mscharged::platform
{
namespace
{
// This file is compiled into the original game module, whose ordinary
// operators allocate from the game heap: keep the scratch images in the host.
template<class T> struct HostAllocator
{
    using value_type = T;
    HostAllocator() noexcept = default;
    template<class U> HostAllocator(const HostAllocator<U>&) noexcept {}
    T* allocate(std::size_t count)
    {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) throw std::bad_alloc();
        void* storage = ChargedNativeMetadataAllocate(count * sizeof(T));
        if (!storage) throw std::bad_alloc();
        return static_cast<T*>(storage);
    }
    void deallocate(T* pointer, std::size_t) noexcept { ChargedNativeMetadataRelease(pointer); }
    template<class U> bool operator==(const HostAllocator<U>&) const noexcept { return true; }
};
template<class T> using HostVector = std::vector<T, HostAllocator<T>>;

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
    std::uint32_t source;      // the kit texture (diffuse of the captain's model)
    std::uint32_t colour;      // the away colour, 0xRRGGBB
    Band strong;               // clearly the kit's colour
    Band weak;                 // its shading and highlights, when connected to strong texels
    bool whiteTrim;            // white details enclosed by kit take the home colour
    const char* references[2]; // teams compared with on team textures
};

namespace
{
// The away colours follow each character's best-known palette swap: Fire
// Mario's white kit with red trim, and contrasting kits for the others that
// none of the game's teams wears.
const AwayKit kKits[] = {
    {"mario", 0xBBFFCF5Eu, 0xF2F2F0u, {345.0f, 12.0f, 0.55f, 0.55f, 0.25f}, {335.0f, 20.0f, 0.22f, 0.45f, 0.18f}, true,
        {"luigi", "waluigi"}},
    {"luigi", 0x4442532Du, 0x3D9BE9u, {95.0f, 150.0f, 0.45f, 0.45f, 0.15f}, {80.0f, 165.0f, 0.15f, 0.15f, 0.05f}, false,
        {"mario", "wario"}},
    {"wario", 0x31A701C7u, 0x2E5CD8u, {40.0f, 58.0f, 0.55f, 0.55f, 0.30f}, {36.0f, 64.0f, 0.30f, 0.30f, 0.10f}, false,
        {"luigi", "waluigi"}},
    {"waluigi", 0xD04A32DFu, 0xF08A24u, {255.0f, 290.0f, 0.45f, 0.45f, 0.18f}, {228.0f, 315.0f, 0.15f, 0.15f, 0.05f}, false,
        {"luigi", "wario"}},
};

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

struct RGB
{
    float r, g, b; // 0..1, sRGB
};
struct Lab
{
    float L, a, b;
};

float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float ToSrgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

// OKLab (Björn Ottosson's published matrices).
Lab ToLab(RGB c)
{
    float r = ToLinear(c.r), g = ToLinear(c.g), b = ToLinear(c.b);
    float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

// Linear sRGB; may lie outside 0..1.
void LabToLinear(Lab c, float out[3])
{
    float l = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float m = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float s = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    out[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    out[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    out[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

// From lightness, chroma and hue (radians), reducing chroma until the colour
// fits sRGB.
RGB FromLch(float L, float C, float h)
{
    L = std::clamp(L, 0.0f, 1.0f);
    float lo = 0.0f, hi = C;
    float rgb[3];
    for (int i = 0; i < 16; ++i)
    {
        float c = i == 0 ? C : 0.5f * (lo + hi);
        LabToLinear({L, c * std::cos(h), c * std::sin(h)}, rgb);
        bool inside = rgb[0] >= -0.0001f && rgb[0] <= 1.0001f && rgb[1] >= -0.0001f && rgb[1] <= 1.0001f
            && rgb[2] >= -0.0001f && rgb[2] <= 1.0001f;
        if (i == 0 && inside) break;
        if (inside)
            lo = c;
        else
            hi = c;
        if (i == 15) LabToLinear({L, lo * std::cos(h), lo * std::sin(h)}, rgb);
    }
    return {ToSrgb(std::clamp(rgb[0], 0.0f, 1.0f)), ToSrgb(std::clamp(rgb[1], 0.0f, 1.0f)),
        ToSrgb(std::clamp(rgb[2], 0.0f, 1.0f))};
}

void ToHsv(RGB c, float& h, float& s, float& v)
{
    float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    v = mx;
    s = mx > 0.0f ? (mx - mn) / mx : 0.0f;
    float d = mx - mn;
    if (d <= 0.0f)
        h = 0.0f;
    else if (mx == c.r)
        h = 60.0f * std::fmod((c.g - c.b) / d + 6.0f, 6.0f);
    else if (mx == c.g)
        h = 60.0f * ((c.b - c.r) / d + 2.0f);
    else
        h = 60.0f * ((c.r - c.g) / d + 4.0f);
}

bool InBand(const Band& band, float h, float s, float v)
{
    bool hue = band.hueLo <= band.hueHi ? (h >= band.hueLo && h <= band.hueHi) : (h >= band.hueLo || h <= band.hueHi);
    return hue && s >= (v >= 0.45f ? band.minSaturation : band.minDarkSaturation) && v >= band.minValue;
}

// ---------------------------------------------------------------- CMPR
struct Texel
{
    RGB colour;
    bool opaque;
};

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

void DecodeBlock(const std::uint8_t* block, Texel out[16])
{
    float palette[4][3];
    bool transparent = BlockPalette(block, palette);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            int index = BlockIndex(block, x, y);
            out[y * 4 + x] = {{palette[index][0] / 255.0f, palette[index][1] / 255.0f, palette[index][2] / 255.0f},
                !(transparent && index == 3)};
        }
}

float Distance2(const float a[3], const float b[3])
{
    float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
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

struct Level
{
    int width, height;
    std::size_t offset; // of the level's tiles in the texture
};

// The CMPR levels of a texture (header included in `texture`).
bool Levels(std::span<const std::uint8_t> texture, HostVector<Level>& levels)
{
    if (texture.size() < 32 || BE32(&texture[4]) != 2) return false;
    std::uint32_t count = BE32(&texture[0]);
    int width = BE16(&texture[14]), height = BE16(&texture[16]);
    if (count == 0 || count > 16 || width <= 0 || height <= 0) return false;
    std::size_t offset = 32;
    for (std::uint32_t l = 0; l < count; ++l)
    {
        std::size_t bytes = std::size_t((width + 7) / 8) * std::size_t((height + 7) / 8) * 32;
        if (offset + bytes > texture.size()) return false;
        levels.push_back({width, height, offset});
        offset += bytes;
        width = std::max(width / 2, 1);
        height = std::max(height / 2, 1);
    }
    return true;
}

// The block holding texel (x, y): tiles of 8x8 texels, four 4x4 blocks each.
std::size_t BlockOffset(const Level& level, int x, int y)
{
    int tilesPerRow = (level.width + 7) / 8;
    int tile = (y / 8) * tilesPerRow + x / 8;
    int sub = ((y / 4) & 1) * 2 + ((x / 4) & 1);
    return level.offset + std::size_t(tile) * 32 + std::size_t(sub) * 8;
}

void DecodeLevel(std::span<const std::uint8_t> texture, const Level& level, HostVector<Texel>& out)
{
    out.assign(std::size_t(level.width) * level.height, Texel{});
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
}

enum : std::uint8_t
{
    kNone = 0,
    kKit = 1,  // recoloured as the away kit
    kTrim = 2, // white trim, takes the home colour
};

// Kit texels of the captain's kit texture: strong texels and the weak ones
// connected to them.
void KitMask(const AwayKit& kit, const Level& level, const HostVector<Texel>& texels, HostVector<std::uint8_t>& mask)
{
    std::size_t count = texels.size();
    mask.assign(count, kNone);
    HostVector<std::uint8_t> weak(count, 0);
    HostVector<int> queue;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!texels[i].opaque) continue;
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

// Team texels of a sidekick or goalie texture: those that differ from both
// references, as only the team colour does, and have the kit's hue (where the
// other teams' emblems differ in shape, this texture is white or grey).
void TeamMask(const AwayKit& kit, const HostVector<Texel>& texels, const HostVector<Texel>& reference0,
    const HostVector<Texel>& reference1, HostVector<std::uint8_t>& mask)
{
    constexpr float kThreshold = 0.07f * 0.07f;
    const Band hue = {kit.weak.hueLo - 10.0f < 0.0f ? kit.weak.hueLo + 350.0f : kit.weak.hueLo - 10.0f,
        kit.weak.hueHi + 10.0f >= 360.0f ? kit.weak.hueHi - 350.0f : kit.weak.hueHi + 10.0f, 0.12f, 0.12f, 0.04f};
    mask.assign(texels.size(), kNone);
    for (std::size_t i = 0; i < texels.size(); ++i)
    {
        if (!texels[i].opaque) continue;
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

bool White(const Texel& t)
{
    float h, s, v;
    ToHsv(t.colour, h, s, v);
    return t.opaque && s < 0.22f && v > 0.70f;
}

// White details enclosed by kit (numbers, outlines, logo circles): white
// regions whose border is mostly kit.
void TrimMask(const Level& level, const HostVector<Texel>& texels, HostVector<std::uint8_t>& mask)
{
    std::size_t count = texels.size();
    HostVector<int> component(count, -1), members;
    for (std::size_t start = 0; start < count; ++start)
    {
        if (component[start] >= 0 || mask[start] != kNone || !White(texels[start])) continue;
        members.clear();
        members.push_back(int(start));
        component[start] = int(start);
        int kitBorder = 0, otherBorder = 0;
        for (std::size_t q = 0; q < members.size(); ++q)
        {
            int i = members[q], x = i % level.width, y = i / level.width;
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k)
            {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= level.width || ny[k] >= level.height)
                {
                    ++otherBorder;
                    continue;
                }
                int j = ny[k] * level.width + nx[k];
                if (component[j] == int(start)) continue;
                if (mask[j] == kKit)
                    ++kitBorder;
                else if (White(texels[j]) && mask[j] == kNone)
                {
                    component[j] = int(start);
                    members.push_back(j);
                }
                else
                    ++otherBorder;
            }
        }
        if (kitBorder > 0 && kitBorder >= 4 * otherBorder)
            for (int i : members) mask[i] = kTrim;
    }
}

struct LchStats
{
    float L, C, h; // medians; h in radians
};

LchStats Median(const HostVector<Texel>& texels, const HostVector<std::uint8_t>& mask, std::uint8_t which)
{
    HostVector<float> L, C, a, b;
    for (std::size_t i = 0; i < texels.size(); ++i)
    {
        if (mask[i] != which) continue;
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

RGB Recolour(RGB colour, const LchStats& from, const LchStats& to)
{
    Lab c = ToLab(colour);
    float C = std::sqrt(c.a * c.a + c.b * c.b);
    // Shading darker than the kit's own colour scales towards black, so
    // shadows next to black lines stay dark; highlights share what lightness
    // remains above the new colour.
    float L;
    if (c.L <= from.L)
        L = from.L > 0.0f ? c.L * (to.L / from.L) : to.L;
    else
        L = to.L + (c.L - from.L) * (1.0f - to.L) / std::max(1.0f - from.L, 0.05f);
    float chroma = from.C > 0.01f ? C * (to.C / from.C) : to.C;
    return FromLch(L, std::max(chroma, 0.0f), to.h);
}

LchStats Target(std::uint32_t rgb)
{
    Lab c = ToLab({((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f});
    return {c.L, std::sqrt(c.a * c.a + c.b * c.b), std::atan2(c.b, c.a)};
}

bool DecodeReference(std::span<const std::uint8_t> reference, const Level& level, std::size_t levelIndex,
    HostVector<Texel>& out)
{
    HostVector<Level> levels;
    if (!Levels(reference, levels) || levels.size() <= levelIndex) return false;
    const Level& other = levels[levelIndex];
    if (other.width != level.width || other.height != level.height) return false;
    DecodeLevel(reference, other, out);
    return true;
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

bool RecolourAwayKit(const AwayKit& kit, AwayKitTexture kind, std::span<std::uint8_t> texture,
    std::span<const std::uint8_t> reference0, std::span<const std::uint8_t> reference1)
{
    HostVector<Level> levels;
    if (!Levels(texture, levels)) return false;
    HostVector<Texel> texels, ref0, ref1;
    HostVector<std::uint8_t> mask;
    const LchStats target = Target(kit.colour);
    LchStats from{}, home{}, trimFrom{};
    for (std::size_t l = 0; l < levels.size(); ++l)
    {
        const Level& level = levels[l];
        DecodeLevel(texture, level, texels);
        if (kind == AwayKitTexture::Kit)
            KitMask(kit, level, texels, mask);
        else
        {
            if (!DecodeReference(reference0, level, l, ref0) || !DecodeReference(reference1, level, l, ref1))
                return false;
            TeamMask(kit, texels, ref0, ref1, mask);
        }
        if (kit.whiteTrim) TrimMask(level, texels, mask);
        // The reference colours come from the full-size level, so every mip
        // level gets the same mapping.
        if (l == 0)
        {
            from = Median(texels, mask, kKit);
            home = from;
            trimFrom = Median(texels, mask, kTrim);
        }
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
                        opaque[i] = decoded[i].opaque;
                        if (bx + x < level.width && by + y < level.height)
                        {
                            std::uint8_t m = mask[std::size_t(by + y) * level.width + bx + x];
                            if (m == kKit)
                            {
                                c = Recolour(c, from, target);
                                touched = true;
                            }
                            else if (m == kTrim)
                            {
                                c = Recolour(c, trimFrom, home);
                                touched = true;
                            }
                        }
                        colours[i][0] = c.r * 255.0f;
                        colours[i][1] = c.g * 255.0f;
                        colours[i][2] = c.b * 255.0f;
                    }
                if (touched) EncodeBlock(colours, opaque, block);
            }
    }
    return true;
}

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
        entry[0] = std::uint8_t(target >> 24);
        entry[1] = std::uint8_t(target >> 16);
        entry[2] = std::uint8_t(target >> 8);
        entry[3] = std::uint8_t(target);
        return recoloured;
    }
    return false;
}
}
