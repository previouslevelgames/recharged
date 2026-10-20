#include "platform/hd_recolour.h"

#include "platform/kit_colour.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace mscharged::platform
{
namespace
{
using kit_colour::Lab;
using kit_colour::LchStats;

constexpr float kPi = 3.14159265358979f;
// The HD image averaged down to the game's size may differ this much (mean
// OKLab distance) from the game's texels: redrawn, not another picture.
constexpr float kMaxMeanDifference = 0.35f;
// Its kit must lie where the game's is: at least this share of the kit's
// texels show the kit's colour there, and elsewhere at most this many more
// (relative to the kit's size) show it where the game's texture does not (a
// picture drawn anew with another framing).
constexpr float kMinKitCoverage = 0.8f;
constexpr float kMaxKitLeak = 0.25f;
// The kit's colour: within this hue of the kit's median, with this share of
// its chroma. Outside the kit it must match closer (a background of a
// neighbouring hue is not the kit).
constexpr float kKitHue = 30.0f * kPi / 180.0f;
constexpr float kKitChromaShare = 0.5f;
constexpr float kLeakHue = 15.0f * kPi / 180.0f;
constexpr float kLeakChromaShare = 0.7f;
constexpr float kLeakLightness = 0.12f;
// Its kit area keeps at least this share of the kit's chroma, within this hue.
constexpr float kMinChromaShare = 0.35f;
constexpr float kMaxHueShift = 35.0f * kPi / 180.0f;
// Grey pixels (anti-aliased edges, white stitching, black lines) follow the mask.
constexpr float kGrey = 0.03f;
// Every other pixel near the kit is kit by its own hue: inside the game's kit
// within the first hue of the kit's, no longer beyond the second (skin next to
// a red kit is about 25 degrees away); next to it, up to one game texel
// outside, closer still.
constexpr float kInsideHueFull = 12.0f * kPi / 180.0f;
constexpr float kInsideHueNone = 22.0f * kPi / 180.0f;
constexpr float kOutsideHueFull = 8.0f * kPi / 180.0f;
constexpr float kOutsideHueNone = 16.0f * kPi / 180.0f;
// A picture drawn anew (its kit found by colour): its kit colour has at least
// this share of the game's kit saturation, is kit within the first hue of the
// kit's (none beyond the second), and covers between these shares of the
// game's kit area (relative to the picture).
constexpr float kColourSaturationShare = 0.45f;
constexpr float kColourHueFull = 10.0f * kPi / 180.0f;
constexpr float kColourHueNone = 18.0f * kPi / 180.0f;
constexpr float kMinColourShare = 0.5f;
constexpr float kMaxColourShare = 2.0f;

struct Rgb
{
    float r, g, b;
};

Rgb Pixel(const std::uint8_t* p) { return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f}; }

float AngleBetween(float a, float b)
{
    float d = std::fabs(a - b);
    return d > kPi ? 2.0f * kPi - d : d;
}

float Median(std::vector<float>& values)
{
    std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(values.size() / 2), values.end());
    return values[values.size() / 2];
}

// Median lightness, chroma and hue (from the median a and b, as the game's recolour does).
LchStats Medians(std::vector<float>& L, std::vector<float>& C, std::vector<float>& a, std::vector<float>& b)
{
    if (L.empty()) return {0.5f, 0.0f, 0.0f};
    const float ma = Median(a), mb = Median(b);
    return {Median(L), Median(C), std::atan2(mb, ma)};
}

float Smoothstep(float lo, float hi, float x)
{
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::uint8_t Byte(float value) { return std::uint8_t(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); }

// A colour's chroma and hue, and its recolour, remembered for recent colours
// (an HD image has far fewer colours than pixels).
class ColourCache
{
public:
    struct Entry
    {
        float chroma, hue, lightness;
        std::array<std::uint8_t, 3> recoloured;
    };
    ColourCache(const LchStats& from, const LchStats& to) : from_(from), to_(to), keys_(65536, kEmpty), entries_(65536) {}
    const Entry& operator()(const std::uint8_t* p)
    {
        const std::uint32_t key = std::uint32_t(p[0]) << 16 | std::uint32_t(p[1]) << 8 | p[2];
        const std::uint32_t slot = (key * 2654435761u) >> 16;
        if (keys_[slot] != key)
        {
            const Lab lab = kit_colour::ToLab(Pixel(p));
            const Rgb c = kit_colour::Recolour(Pixel(p), from_, to_);
            keys_[slot] = key;
            entries_[slot] = {std::hypot(lab.a, lab.b), std::atan2(lab.b, lab.a), lab.L, {Byte(c.r), Byte(c.g), Byte(c.b)}};
        }
        return entries_[slot];
    }

private:
    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;
    LchStats from_, to_;
    std::vector<std::uint32_t> keys_; // on the heap: worker threads have small stacks
    std::vector<Entry> entries_;
};
// Saturation as chroma over lightness: the kit's shades keep theirs (dark red
// cloth as much as red cloth), skin and highlights have far less.
float Saturation(float chroma, float lightness) { return chroma / std::max(lightness, 0.05f); }

// A picture the pack drew anew: its kit, wherever it lies, is the pixels of
// the kit's colour (as the game finds a picture's kit by colour too).
// `gameShare`: the share of the game's picture that is kit.
bool RecolourByColour(const LchStats& original, float gameShare, const KitRecolour& kit, RgbaImage& image,
                      std::string& why)
{
    const std::size_t pixels = std::size_t(image.width) * image.height;
    const std::size_t step = std::max<std::size_t>(1, pixels / 262144);
    const float originalSaturation = Saturation(original.C, original.L);
    // The pack's kit colour: the pixels near the game's kit colour.
    std::vector<float> L, C, a, b, S;
    std::size_t opaque = 0;
    for (std::size_t p = 0; p < pixels; p += step)
    {
        if (image.rgba[p * 4 + 3] < 128) continue;
        ++opaque;
        const Lab c = kit_colour::ToLab(Pixel(&image.rgba[p * 4]));
        const float chroma = std::hypot(c.a, c.b);
        if (Saturation(chroma, c.L) < kColourSaturationShare * originalSaturation
            || AngleBetween(std::atan2(c.b, c.a), original.h) > kMaxHueShift)
            continue;
        L.push_back(c.L), C.push_back(chroma), a.push_back(c.a), b.push_back(c.b), S.push_back(Saturation(chroma, c.L));
    }
    if (L.empty() || !opaque)
    {
        why = "it has no kit colour";
        return false;
    }
    const LchStats from = Medians(L, C, a, b);
    const float saturation = Median(S);
    // A pixel's share of being kit: by hue, and by saturation against the kit's.
    auto kitWeight = [&](float hue, float chroma, float lightness) {
        if (chroma < kGrey) return 0.0f;
        const float byHue = std::clamp((kColourHueNone - AngleBetween(hue, from.h)) / (kColourHueNone - kColourHueFull),
                                       0.0f, 1.0f);
        return byHue * Smoothstep(0.45f, 0.7f, Saturation(chroma, lightness) / std::max(saturation, 0.01f));
    };
    std::size_t kitPixels = 0;
    for (std::size_t p = 0; p < pixels; p += step)
    {
        if (image.rgba[p * 4 + 3] < 128) continue;
        const Lab c = kit_colour::ToLab(Pixel(&image.rgba[p * 4]));
        kitPixels += kitWeight(std::atan2(c.b, c.a), std::hypot(c.a, c.b), c.L) >= 0.5f;
    }
    // About as much kit as the game's picture (a background of the kit's
    // colour would be much more).
    const float share = float(kitPixels) / float(opaque);
    if (share < kMinColourShare * gameShare || share > kMaxColourShare * gameShare)
    {
        why = "its kit colour covers " + std::to_string(int(100.0f * share)) + "% of it, the game's kit "
            + std::to_string(int(100.0f * gameShare)) + "%";
        return false;
    }
    ColourCache colours(from, LchStats{kit.to[0], kit.to[1], kit.to[2]});
    for (std::size_t p = 0; p < pixels; ++p)
    {
        std::uint8_t* px = &image.rgba[p * 4];
        if (px[3] == 0) continue;
        const auto& colour = colours(px);
        const float weight = kitWeight(colour.hue, colour.chroma, colour.lightness);
        if (weight <= 0.0f) continue;
        for (int k = 0; k < 3; ++k)
            px[k] = std::uint8_t(std::lround(px[k] + (float(colour.recoloured[k]) - px[k]) * weight));
    }
    return true;
}
} // namespace

bool RecolourHdImage(const KitRecolour& kit, RgbaImage& image, std::string* why)
{
    auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    const std::uint32_t w = kit.width, h = kit.height, W = image.width, H = image.height;
    if (!w || !h || kit.mask.size() != std::size_t(w) * h || kit.rgba.size() != std::size_t(w) * h * 4
        || image.rgba.size() != std::size_t(W) * H * 4)
        return fail("incomplete data");
    if (W < w || H < h || std::uint64_t(W) * h != std::uint64_t(H) * w)
        return fail("its size does not match the game's texture");
    const double sx = double(W) / w, sy = double(H) / h;

    // The HD image averaged down to the game's size, against the game's texels.
    std::vector<float> oL, oC, oa, ob, hL, hC, ha, hb;
    std::vector<Lab> originalTexels(std::size_t(w) * h), drawnTexels(std::size_t(w) * h);
    std::vector<std::uint8_t> opaque(std::size_t(w) * h, 0);
    double difference = 0.0;
    std::size_t compared = 0;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
        {
            const std::size_t i = std::size_t(y) * w + x;
            if (kit.rgba[i * 4 + 3] < 128) continue;
            const std::uint32_t x0 = std::uint32_t(x * sx), x1 = std::max(x0 + 1, std::uint32_t((x + 1) * sx));
            const std::uint32_t y0 = std::uint32_t(y * sy), y1 = std::max(y0 + 1, std::uint32_t((y + 1) * sy));
            float sum[3] = {};
            for (std::uint32_t Y = y0; Y < y1 && Y < H; ++Y)
                for (std::uint32_t X = x0; X < x1 && X < W; ++X)
                    for (int c = 0; c < 3; ++c) sum[c] += image.rgba[(std::size_t(Y) * W + X) * 4 + c];
            const float n = float((std::min(x1, W) - x0) * (std::min(y1, H) - y0)) * 255.0f;
            const Lab o = kit_colour::ToLab(Pixel(&kit.rgba[i * 4]));
            const Lab d = kit_colour::ToLab(Rgb{sum[0] / n, sum[1] / n, sum[2] / n});
            difference += std::sqrt((o.L - d.L) * (o.L - d.L) + (o.a - d.a) * (o.a - d.a) + (o.b - d.b) * (o.b - d.b));
            ++compared;
            originalTexels[i] = o, drawnTexels[i] = d, opaque[i] = 1;
            if (!kit.mask[i]) continue;
            oL.push_back(o.L), oC.push_back(std::hypot(o.a, o.b)), oa.push_back(o.a), ob.push_back(o.b);
            hL.push_back(d.L), hC.push_back(std::hypot(d.a, d.b)), ha.push_back(d.a), hb.push_back(d.b);
        }
    if (oL.empty()) return fail("no kit texels");
    if (compared && difference / double(compared) > kMaxMeanDifference) return fail("the picture differs from the game's");
    const std::size_t gameKit = oL.size();
    const LchStats original = Medians(oL, oC, oa, ob), drawn = Medians(hL, hC, ha, hb);
    std::string misplaced; // why the pack's kit does not lie where the game's does
    if (original.C > 0.04f && drawn.C < kMinChromaShare * original.C) misplaced = "its kit lacks the kit's colour";
    else if (original.C > 0.04f && AngleBetween(original.h, drawn.h) > kMaxHueShift)
        misplaced = "its kit has another colour";
    else if (original.C > 0.04f)
    {
        auto kitColoured = [](const Lab& c, const LchStats& colour) {
            return std::hypot(c.a, c.b) >= kKitChromaShare * colour.C
                && AngleBetween(std::atan2(c.b, c.a), colour.h) <= kKitHue;
        };
        std::size_t kitTexels = 0, covered = 0, leaked = 0;
        for (std::size_t i = 0; i < opaque.size(); ++i)
        {
            if (!opaque[i]) continue;
            const bool there = kitColoured(drawnTexels[i], drawn);
            const Lab& d = drawnTexels[i];
            const bool kitExactly = std::hypot(d.a, d.b) >= kLeakChromaShare * drawn.C
                && AngleBetween(std::atan2(d.b, d.a), drawn.h) <= kLeakHue && std::fabs(d.L - drawn.L) <= kLeakLightness;
            if (kit.mask[i])
            {
                // The kit's dark shading has little chroma in either.
                ++kitTexels;
                covered += there
                    || (!kitColoured(originalTexels[i], original) && std::hypot(drawnTexels[i].a, drawnTexels[i].b) < 3 * kGrey);
            }
            else if (kitExactly && !kitColoured(originalTexels[i], original))
                ++leaked;
        }
        auto share = [&](std::size_t count) { return std::to_string(int(100.0f * float(count) / float(kitTexels))) + "%"; };
        if (float(covered) < kMinKitCoverage * float(kitTexels))
            misplaced = "its kit is not where the game's is (" + share(covered) + " of it)";
        else if (float(leaked) > kMaxKitLeak * float(kitTexels))
            misplaced = "it shows the kit's colour where the game's does not (" + share(leaked) + " more)";
    }
    if (!misplaced.empty())
    {
        if (!kit.picture || original.C <= 0.04f) return fail(misplaced);
        std::string colourWhy;
        if (!RecolourByColour(original, float(gameKit) / float(std::max<std::size_t>(compared, 1)), kit, image,
                              colourWhy))
            return fail(misplaced + ", and by colour " + colourWhy);
        if (why) *why = "by colour: the pack's picture is drawn anew (" + misplaced + ")";
        return true;
    }

    // The kit grown by one texel: HD kit pixels just past the game's kit edge.
    std::vector<std::uint8_t> grown(kit.mask);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            for (int dy = -1; dy <= 1 && !grown[std::size_t(y) * w + x]; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const auto nx = std::int64_t(x) + dx, ny = std::int64_t(y) + dy;
                    if (nx >= 0 && ny >= 0 && nx < w && ny < h && kit.mask[std::size_t(ny) * w + std::size_t(nx)])
                    {
                        grown[std::size_t(y) * w + x] = 1;
                        break;
                    }
                }
    // A mask at an HD pixel: bilinear between the game's texel centres.
    auto sample = [&](const std::vector<std::uint8_t>& mask, std::uint32_t X, std::uint32_t Y) {
        const double u = (X + 0.5) / sx - 0.5, v = (Y + 0.5) / sy - 0.5;
        const double fu = std::floor(u), fv = std::floor(v);
        const float tu = float(u - fu), tv = float(v - fv);
        auto at = [&](double x, double y) {
            const auto cx = std::uint32_t(std::clamp(x, 0.0, double(w - 1)));
            const auto cy = std::uint32_t(std::clamp(y, 0.0, double(h - 1)));
            return mask[std::size_t(cy) * w + cx] ? 1.0f : 0.0f;
        };
        const float top = at(fu, fv) * (1.0f - tu) + at(fu + 1, fv) * tu;
        const float bottom = at(fu, fv + 1) * (1.0f - tu) + at(fu + 1, fv + 1) * tu;
        return top * (1.0f - tv) + bottom * tv;
    };

    auto maskAt = [&](std::uint32_t X, std::uint32_t Y) { return sample(kit.mask, X, Y); };

    // The HD kit's own colour (its kit-coloured pixels well inside the kit),
    // so its shading maps to the away colour as the game's texels do.
    std::vector<float> kL, kC, ka, kb;
    const std::size_t step = std::max<std::size_t>(1, std::size_t(W) * H / 262144);
    for (std::size_t p = 0; p < std::size_t(W) * H; p += step)
    {
        const auto X = std::uint32_t(p % W), Y = std::uint32_t(p / W);
        if (image.rgba[p * 4 + 3] == 0 || maskAt(X, Y) < 1.0f) continue;
        const Lab c = kit_colour::ToLab(Pixel(&image.rgba[p * 4]));
        if (std::hypot(c.a, c.b) >= kGrey && AngleBetween(std::atan2(c.b, c.a), drawn.h) > kKitHue) continue;
        kL.push_back(c.L), kC.push_back(std::hypot(c.a, c.b)), ka.push_back(c.a), kb.push_back(c.b);
    }
    const LchStats from = kL.empty() ? drawn : Medians(kL, kC, ka, kb);
    const LchStats to{kit.to[0], kit.to[1], kit.to[2]};

    ColourCache colours(from, to);
    for (std::uint32_t Y = 0; Y < H; ++Y)
        for (std::uint32_t X = 0; X < W; ++X)
        {
            std::uint8_t* p = &image.rgba[(std::size_t(Y) * W + X) * 4];
            if (p[3] == 0 || sample(grown, X, Y) <= 0.0f) continue;
            const float m = maskAt(X, Y);
            const auto& colour = colours(p);
            float weight;
            if (colour.chroma < kGrey)
                weight = Smoothstep(0.25f, 0.75f, m);
            else
            {
                const bool inside = m >= 0.5f;
                const float full = inside ? kInsideHueFull : kOutsideHueFull;
                const float none = inside ? kInsideHueNone : kOutsideHueNone;
                weight = std::clamp((none - AngleBetween(colour.hue, from.h)) / (none - full), 0.0f, 1.0f);
            }
            if (weight <= 0.0f) continue;
            for (int k = 0; k < 3; ++k)
                p[k] = std::uint8_t(std::lround(p[k] + (float(colour.recoloured[k]) - p[k]) * weight));
        }
    return true;
}

std::vector<std::uint8_t> EncodeDds(const RgbaImage& image, bool mips)
{
    std::vector<RgbaImage> levels{image};
    while (mips && (levels.back().width > 1 || levels.back().height > 1))
    {
        const RgbaImage& from = levels.back();
        RgbaImage next;
        next.width = std::max(from.width / 2, 1u);
        next.height = std::max(from.height / 2, 1u);
        next.rgba.resize(std::size_t(next.width) * next.height * 4);
        for (std::uint32_t y = 0; y < next.height; ++y)
            for (std::uint32_t x = 0; x < next.width; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    unsigned sum = 0, n = 0;
                    for (std::uint32_t dy = 0; dy < 2; ++dy)
                        for (std::uint32_t dx = 0; dx < 2; ++dx)
                        {
                            const std::uint32_t sx = std::min(x * 2 + dx, from.width - 1);
                            const std::uint32_t sy = std::min(y * 2 + dy, from.height - 1);
                            sum += from.rgba[(std::size_t(sy) * from.width + sx) * 4 + c];
                            ++n;
                        }
                    next.rgba[(std::size_t(y) * next.width + x) * 4 + c] = std::uint8_t((sum + n / 2) / n);
                }
        levels.push_back(std::move(next));
    }

    std::uint32_t header[32] = {};
    header[0] = 0x20534444;                                        // "DDS "
    header[1] = 124;                                               // header size
    header[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (mips ? 0x20000 : 0); // caps, height, width, pitch, format, mip count
    header[3] = image.height;
    header[4] = image.width;
    header[5] = image.width * 4;                                   // pitch
    header[7] = std::uint32_t(levels.size());
    header[19] = 32;                                               // pixel format size
    header[20] = 0x41;                                             // RGB with alpha
    header[22] = 32;                                               // bits per pixel
    header[23] = 0x000000FF;
    header[24] = 0x0000FF00;
    header[25] = 0x00FF0000;
    header[26] = 0xFF000000;
    header[27] = 0x1000 | (mips ? 0x400000 | 0x8 : 0);             // texture, mipmap, complex
    std::vector<std::uint8_t> out(sizeof header);
    for (int i = 0; i < 32; ++i)
        for (int b = 0; b < 4; ++b) out[std::size_t(i) * 4 + b] = std::uint8_t(header[i] >> (8 * b));
    for (const auto& level : levels) out.insert(out.end(), level.rgba.begin(), level.rgba.end());
    return out;
}
}
