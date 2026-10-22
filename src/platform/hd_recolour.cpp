#include "platform/hd_recolour.h"

#include "platform/kit_colour.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <system_error>
#include <thread>

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
// A picture redrawn with the game's framing (the lightness of its texels
// follows the game's: correlation at least this) may shade parts of its kit
// in another colour (a stripe): this share of the kit's texels is enough then.
constexpr float kMinFramingCorrelation = 0.75f;
constexpr float kMinRedrawnKitCoverage = 0.6f;
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

// A pixel's OKLab, as kit_colour::ToLab of its bytes / 255, with the sRGB
// curve looked up for each byte value.
Lab LabOf(const std::uint8_t* p)
{
    static const std::array<float, 256> linear = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i) table[i] = kit_colour::ToLinear(i / 255.0f);
        return table;
    }();
    return kit_colour::LinearToLab(linear[p[0]], linear[p[1]], linear[p[2]]);
}

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

// The threads that may work on an image: `threads`, or with 0 as many as the
// machine has (at most 8).
std::size_t Parts(unsigned threads)
{
    const unsigned machine = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    return threads ? std::min(threads, machine) : machine;
}

// Runs work(part, begin, end) over [0, count) in contiguous parts of at least
// `grain` items, at most `most` of them, on threads of their own and the
// calling one. Rethrows a part's exception once all are done.
template<class Work> void InParts(std::size_t most, std::size_t count, std::size_t grain, const Work& work)
{
    const std::size_t parts = std::clamp<std::size_t>(count / grain, 1, most);
    const std::size_t size = (count + parts - 1) / parts;
    std::vector<std::exception_ptr> errors(parts);
    auto run = [&](std::size_t part) {
        try
        {
            work(part, std::min(count, part * size), std::min(count, (part + 1) * size));
        }
        catch (...)
        {
            errors[part] = std::current_exception();
        }
    };
    std::vector<std::thread> helpers;
    helpers.reserve(parts - 1);
    std::size_t part = 0;
    for (; part + 1 < parts; ++part)
    {
        try
        {
            helpers.emplace_back(run, part);
        }
        catch (const std::system_error&)
        {
            break; // no thread: the rest runs here
        }
    }
    for (; part < parts; ++part) run(part);
    for (auto& helper : helpers) helper.join();
    for (const auto& error : errors)
        if (error) std::rethrow_exception(error);
}

// Pixels of an image (or texels of a texture) per thread at least.
constexpr std::size_t kGrain = 16384;

// A colour's chroma and hue, and its recolour (made when first asked: most
// pixels are not kit), remembered for recent colours (an HD image has far
// fewer colours than pixels).
class ColourCache
{
public:
    struct Entry
    {
        Lab lab;
        float chroma, hue;
        bool made;
        std::array<std::uint8_t, 3> recoloured;
    };
    ColourCache(const LchStats& from, const LchStats& to) : from_(from), to_(to), keys_(65536, kEmpty), entries_(65536) {}
    Entry& operator()(const std::uint8_t* p)
    {
        const std::uint32_t key = std::uint32_t(p[0]) << 16 | std::uint32_t(p[1]) << 8 | p[2];
        const std::uint32_t slot = (key * 2654435761u) >> 16;
        if (keys_[slot] != key)
        {
            const Lab lab = LabOf(p);
            keys_[slot] = key;
            entries_[slot] = {lab, std::hypot(lab.a, lab.b), std::atan2(lab.b, lab.a), false, {}};
        }
        return entries_[slot];
    }
    const std::array<std::uint8_t, 3>& Recoloured(Entry& entry)
    {
        if (!entry.made)
        {
            const Rgb c = kit_colour::RecolourLab<Rgb>(entry.lab, from_, to_);
            entry.recoloured = {Byte(c.r), Byte(c.g), Byte(c.b)};
            entry.made = true;
        }
        return entry.recoloured;
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
                      std::size_t parts, std::string& why)
{
    const std::size_t pixels = std::size_t(image.width) * image.height;
    const std::size_t step = std::max<std::size_t>(1, pixels / 262144);
    const float originalSaturation = Saturation(original.C, original.L);
    const std::size_t samples = (pixels + step - 1) / step;
    // The pack's kit colour: the pixels near the game's kit colour.
    struct Found
    {
        std::vector<float> L, C, a, b, S;
        std::size_t opaque = 0;
    };
    std::vector<Found> found(parts);
    InParts(parts, samples, kGrain, [&](std::size_t part, std::size_t begin, std::size_t end) {
        Found& f = found[part];
        for (std::size_t p = begin * step; p < end * step; p += step)
        {
            if (image.rgba[p * 4 + 3] < 128) continue;
            ++f.opaque;
            const Lab c = LabOf(&image.rgba[p * 4]);
            const float chroma = std::hypot(c.a, c.b);
            if (Saturation(chroma, c.L) < kColourSaturationShare * originalSaturation
                || AngleBetween(std::atan2(c.b, c.a), original.h) > kMaxHueShift)
                continue;
            f.L.push_back(c.L), f.C.push_back(chroma), f.a.push_back(c.a), f.b.push_back(c.b);
            f.S.push_back(Saturation(chroma, c.L));
        }
    });
    std::vector<float> L, C, a, b, S;
    std::size_t opaque = 0;
    for (const Found& f : found)
    {
        L.insert(L.end(), f.L.begin(), f.L.end()), C.insert(C.end(), f.C.begin(), f.C.end());
        a.insert(a.end(), f.a.begin(), f.a.end()), b.insert(b.end(), f.b.begin(), f.b.end());
        S.insert(S.end(), f.S.begin(), f.S.end());
        opaque += f.opaque;
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
    std::vector<std::size_t> kitCounts(found.size(), 0);
    InParts(parts, samples, kGrain, [&](std::size_t part, std::size_t begin, std::size_t end) {
        for (std::size_t p = begin * step; p < end * step; p += step)
        {
            if (image.rgba[p * 4 + 3] < 128) continue;
            const Lab c = LabOf(&image.rgba[p * 4]);
            kitCounts[part] += kitWeight(std::atan2(c.b, c.a), std::hypot(c.a, c.b), c.L) >= 0.5f;
        }
    });
    std::size_t kitPixels = 0;
    for (const std::size_t count : kitCounts) kitPixels += count;
    // About as much kit as the game's picture (a background of the kit's
    // colour would be much more).
    const float share = float(kitPixels) / float(opaque);
    if (share < kMinColourShare * gameShare || share > kMaxColourShare * gameShare)
    {
        why = "its kit colour covers " + std::to_string(int(100.0f * share)) + "% of it, the game's kit "
            + std::to_string(int(100.0f * gameShare)) + "%";
        return false;
    }
    InParts(parts, pixels, kGrain, [&](std::size_t, std::size_t begin, std::size_t end) {
        ColourCache colours(from, LchStats{kit.to[0], kit.to[1], kit.to[2]});
        for (std::size_t p = begin; p < end; ++p)
        {
            std::uint8_t* px = &image.rgba[p * 4];
            if (px[3] == 0) continue;
            auto& colour = colours(px);
            const float weight = kitWeight(colour.hue, colour.chroma, colour.lab.L);
            if (weight <= 0.0f) continue;
            const auto& recoloured = colours.Recoloured(colour);
            for (int k = 0; k < 3; ++k)
                px[k] = std::uint8_t(std::lround(px[k] + (float(recoloured[k]) - px[k]) * weight));
        }
    });
    return true;
}
} // namespace

bool RecolourHdImage(const KitRecolour& kit, RgbaImage& image, std::string* why, unsigned threads)
{
    const std::size_t parts = Parts(threads);
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
    InParts(parts, std::size_t(w) * h, kGrain / 4, [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i)
        {
            if (kit.rgba[i * 4 + 3] < 128) continue;
            const auto x = std::uint32_t(i % w), y = std::uint32_t(i / w);
            const std::uint32_t x0 = std::uint32_t(x * sx), x1 = std::max(x0 + 1, std::uint32_t((x + 1) * sx));
            const std::uint32_t y0 = std::uint32_t(y * sy), y1 = std::max(y0 + 1, std::uint32_t((y + 1) * sy));
            float sum[3] = {};
            for (std::uint32_t Y = y0; Y < y1 && Y < H; ++Y)
                for (std::uint32_t X = x0; X < x1 && X < W; ++X)
                    for (int c = 0; c < 3; ++c) sum[c] += image.rgba[(std::size_t(Y) * W + X) * 4 + c];
            const float n = float((std::min(x1, W) - x0) * (std::min(y1, H) - y0)) * 255.0f;
            originalTexels[i] = LabOf(&kit.rgba[i * 4]);
            drawnTexels[i] = kit_colour::ToLab(Rgb{sum[0] / n, sum[1] / n, sum[2] / n});
            opaque[i] = 1;
        }
    });
    double difference = 0.0;
    std::size_t compared = 0;
    for (std::size_t i = 0; i < opaque.size(); ++i)
    {
        if (!opaque[i]) continue;
        const Lab o = originalTexels[i], d = drawnTexels[i];
        difference += std::sqrt((o.L - d.L) * (o.L - d.L) + (o.a - d.a) * (o.a - d.a) + (o.b - d.b) * (o.b - d.b));
        ++compared;
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
        std::size_t kitTexels = 0, covered = 0, leaked = 0, texels = 0;
        double o1 = 0, d1 = 0, oo = 0, dd = 0, od = 0; // lightness sums
        for (std::size_t i = 0; i < opaque.size(); ++i)
        {
            if (!opaque[i]) continue;
            const bool there = kitColoured(drawnTexels[i], drawn);
            const Lab& d = drawnTexels[i];
            const double oL = originalTexels[i].L, dL = d.L;
            ++texels, o1 += oL, d1 += dL, oo += oL * oL, dd += dL * dL, od += oL * dL;
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
        const double n = double(texels), oVar = oo / n - o1 / n * (o1 / n), dVar = dd / n - d1 / n * (d1 / n);
        const bool sameFraming = oVar > 1e-9 && dVar > 1e-9
            && (od / n - o1 / n * (d1 / n)) / std::sqrt(oVar * dVar) >= kMinFramingCorrelation;
        auto share = [&](std::size_t count) { return std::to_string(int(100.0f * float(count) / float(kitTexels))) + "%"; };
        if (float(covered) < (sameFraming ? kMinRedrawnKitCoverage : kMinKitCoverage) * float(kitTexels))
            misplaced = "its kit is not where the game's is (" + share(covered) + " of it)";
        else if (float(leaked) > kMaxKitLeak * float(kitTexels))
            misplaced = "it shows the kit's colour where the game's does not (" + share(leaked) + " more)";
    }
    if (!misplaced.empty())
    {
        if (!kit.picture || original.C <= 0.04f) return fail(misplaced);
        std::string colourWhy;
        if (!RecolourByColour(original, float(gameKit) / float(std::max<std::size_t>(compared, 1)), kit, image, parts,
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
    const std::size_t step = std::max<std::size_t>(1, std::size_t(W) * H / 262144);
    struct Found
    {
        std::vector<float> L, C, a, b;
    };
    std::vector<Found> found(parts);
    InParts(parts, (std::size_t(W) * H + step - 1) / step, kGrain, [&](std::size_t part, std::size_t begin, std::size_t end) {
        Found& f = found[part];
        for (std::size_t p = begin * step; p < end * step; p += step)
        {
            const auto X = std::uint32_t(p % W), Y = std::uint32_t(p / W);
            if (image.rgba[p * 4 + 3] == 0 || maskAt(X, Y) < 1.0f) continue;
            const Lab c = LabOf(&image.rgba[p * 4]);
            if (std::hypot(c.a, c.b) >= kGrey && AngleBetween(std::atan2(c.b, c.a), drawn.h) > kKitHue) continue;
            f.L.push_back(c.L), f.C.push_back(std::hypot(c.a, c.b)), f.a.push_back(c.a), f.b.push_back(c.b);
        }
    });
    std::vector<float> kL, kC, ka, kb;
    for (const Found& f : found)
    {
        kL.insert(kL.end(), f.L.begin(), f.L.end()), kC.insert(kC.end(), f.C.begin(), f.C.end());
        ka.insert(ka.end(), f.a.begin(), f.a.end()), kb.insert(kb.end(), f.b.begin(), f.b.end());
    }
    const LchStats from = kL.empty() ? drawn : Medians(kL, kC, ka, kb);
    const LchStats to{kit.to[0], kit.to[1], kit.to[2]};

    InParts(parts, H, std::max<std::size_t>(1, kGrain / W), [&](std::size_t, std::size_t begin, std::size_t end) {
        ColourCache colours(from, to);
        for (auto Y = std::uint32_t(begin); Y < end; ++Y)
            for (std::uint32_t X = 0; X < W; ++X)
            {
                std::uint8_t* p = &image.rgba[(std::size_t(Y) * W + X) * 4];
                if (p[3] == 0 || sample(grown, X, Y) <= 0.0f) continue;
                const float m = maskAt(X, Y);
                auto& colour = colours(p);
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
                const auto& recoloured = colours.Recoloured(colour);
                for (int k = 0; k < 3; ++k)
                    p[k] = std::uint8_t(std::lround(p[k] + (float(recoloured[k]) - p[k]) * weight));
            }
    });
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
        {
            // The 2x2 texels below (the last row or column again at an odd edge).
            const std::uint8_t* top = &from.rgba[std::size_t(std::min(y * 2, from.height - 1)) * from.width * 4];
            const std::uint8_t* bottom = &from.rgba[std::size_t(std::min(y * 2 + 1, from.height - 1)) * from.width * 4];
            std::uint8_t* out = &next.rgba[std::size_t(y) * next.width * 4];
            for (std::uint32_t x = 0; x < next.width; ++x)
            {
                const std::size_t left = std::size_t(std::min(x * 2, from.width - 1)) * 4;
                const std::size_t right = std::size_t(std::min(x * 2 + 1, from.width - 1)) * 4;
                for (int c = 0; c < 4; ++c)
                    out[x * 4 + c] =
                        std::uint8_t((unsigned(top[left + c]) + top[right + c] + bottom[left + c] + bottom[right + c] + 2) / 4);
            }
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
    std::size_t bytes = sizeof header;
    for (const auto& level : levels) bytes += level.rgba.size();
    std::vector<std::uint8_t> out(sizeof header);
    out.reserve(bytes);
    for (int i = 0; i < 32; ++i)
        for (int b = 0; b < 4; ++b) out[std::size_t(i) * 4 + b] = std::uint8_t(header[i] >> (8 * b));
    for (const auto& level : levels) out.insert(out.end(), level.rgba.begin(), level.rgba.end());
    return out;
}
}
