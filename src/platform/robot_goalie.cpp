#include "platform/robot_goalie.h"

#include "platform/gx_texture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mscharged::platform
{
using namespace gx;

namespace
{
std::uint32_t LE32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint16_t LE16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }

constexpr int kMaxTargets = 8, kMaxSide = 1024, kMaxSamples = 4;

struct Target
{
    std::uint32_t texture = 0;
    int width = 0, height = 0;
    const std::uint8_t* counts = nullptr;  // samples per texel
    const std::uint8_t* samples = nullptr; // (u16 s, u16 t), 65535 = 1
};

// The map's targets, or false. Layout: assets/gameplay/README.md.
bool Parse(std::span<const std::uint8_t> map, HostVector<Target>& targets)
{
    targets.clear();
    if (map.size() < 16 || std::memcmp(map.data(), "RGM1", 4) != 0 || LE32(&map[4]) != 1
        || LE32(&map[8]) != kRobotGoalieTexture)
        return false;
    const std::uint32_t count = LE32(&map[12]);
    if (count == 0 || count > kMaxTargets) return false;
    std::size_t at = 16;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (map.size() - at < 12) return false;
        Target t;
        t.texture = LE32(&map[at]);
        t.width = LE16(&map[at + 4]);
        t.height = LE16(&map[at + 6]);
        const std::uint32_t samples = LE32(&map[at + 8]);
        at += 12;
        if (t.width < 1 || t.height < 1 || t.width > kMaxSide || t.height > kMaxSide) return false;
        const std::size_t texels = std::size_t(t.width) * t.height;
        if (map.size() - at < texels) return false;
        t.counts = &map[at];
        std::uint64_t total = 0;
        for (std::size_t k = 0; k < texels; ++k)
        {
            if (t.counts[k] < 1 || t.counts[k] > kMaxSamples) return false;
            total += t.counts[k];
        }
        at += texels;
        if (total != samples || (map.size() - at) / 4 < total) return false;
        t.samples = &map[at];
        at += std::size_t(total) * 4;
        for (const Target& other : targets)
            if (other.texture == t.texture) return false;
        targets.push_back(t);
    }
    return at == map.size();
}

// nlStringHash, the game's texture names.
std::uint32_t NameHash(const char* name)
{
    std::uint32_t hash = 0xFFFFFFFFu;
    for (; *name; ++name) hash = hash + (hash << 5) + std::uint8_t(*name);
    return hash;
}

// Every team's goalie kit and away kit share the layout of Mario's.
bool IsGoalieKit(std::uint32_t texture)
{
    static const char* const kTeams[] = {"mario", "bowser", "daisy", "donkeykong", "luigi", "peach",
        "waluigi", "wario", "yoshi", "bowserjr", "diddykong", "petey"};
    char name[64];
    for (const char* team : kTeams)
    {
        std::snprintf(name, sizeof name, "%sgoalie/%sgoalie", team, team);
        if (NameHash(name) == texture) return true;
        std::snprintf(name, sizeof name, "%sgoalie_alt/%sgoalie_alt", team, team);
        if (NameHash(name) == texture) return true;
    }
    return false;
}

struct Image
{
    int width = 0, height = 0;
    HostVector<float> rgb; // 0..255
};

// A target's texels: the average of its samples of the robot texture (bilinear, repeating).
Image Make(const Target& target, const Level& source, const HostVector<Texel>& texels)
{
    Image out;
    out.width = target.width;
    out.height = target.height;
    out.rgb.assign(std::size_t(target.width) * target.height * 3, 0.0f);
    auto at = [&](int x, int y) -> const RGB& {
        x = ((x % source.width) + source.width) % source.width;
        y = ((y % source.height) + source.height) % source.height;
        return texels[std::size_t(y) * source.width + x].colour;
    };
    const std::uint8_t* sample = target.samples;
    for (std::size_t k = 0; k < std::size_t(target.width) * target.height; ++k)
    {
        float sum[3] = {0, 0, 0};
        for (int i = 0; i < target.counts[k]; ++i, sample += 4)
        {
            float x = LE16(sample) / 65535.0f * source.width - 0.5f, y = LE16(sample + 2) / 65535.0f * source.height - 0.5f;
            int x0 = int(std::floor(x)), y0 = int(std::floor(y));
            float fx = x - x0, fy = y - y0;
            const RGB &a = at(x0, y0), &b = at(x0 + 1, y0), &c = at(x0, y0 + 1), &d = at(x0 + 1, y0 + 1);
            sum[0] += (a.r * (1 - fx) + b.r * fx) * (1 - fy) + (c.r * (1 - fx) + d.r * fx) * fy;
            sum[1] += (a.g * (1 - fx) + b.g * fx) * (1 - fy) + (c.g * (1 - fx) + d.g * fx) * fy;
            sum[2] += (a.b * (1 - fx) + b.b * fx) * (1 - fy) + (c.b * (1 - fx) + d.b * fx) * fy;
        }
        for (int c = 0; c < 3; ++c) out.rgb[k * 3 + c] = sum[c] / target.counts[k] * 255.0f;
    }
    return out;
}

// The shine mask of a metal robot: bright where the made kit is, never dull.
Image ShineMask(const Image& kit)
{
    Image out = kit;
    for (std::size_t k = 0; k < out.rgb.size(); k += 3)
    {
        float luma = (0.2126f * kit.rgb[k] + 0.7152f * kit.rgb[k + 1] + 0.0722f * kit.rgb[k + 2]) / 255.0f;
        float shine = std::clamp(0.3f + 1.2f * luma, 0.0f, 1.0f) * 255.0f;
        out.rgb[k] = out.rgb[k + 1] = out.rgb[k + 2] = shine;
    }
    return out;
}

Image Half(const Image& in)
{
    Image out;
    out.width = std::max(in.width / 2, 1);
    out.height = std::max(in.height / 2, 1);
    out.rgb.assign(std::size_t(out.width) * out.height * 3, 0.0f);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < 3; ++c)
            {
                float sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        int sx = std::min(x * 2 + dx, in.width - 1), sy = std::min(y * 2 + dy, in.height - 1);
                        sum += in.rgb[(std::size_t(sy) * in.width + sx) * 3 + c];
                    }
                out.rgb[(std::size_t(y) * out.width + x) * 3 + c] = sum / 4;
            }
    return out;
}

// Writes `image` over a CMPR texture of its size, every level; transparent
// texels of the original stay transparent.
bool Write(std::span<std::uint8_t> texture, const Image& image)
{
    HostVector<Level> levels;
    std::uint32_t format;
    if (!Levels(texture, format, levels) || format != kCMPR || levels[0].width != image.width
        || levels[0].height != image.height)
        return false;
    Image level = image;
    for (std::size_t l = 0; l < levels.size(); ++l)
    {
        if (l) level = Half(level);
        const Level& target = levels[l];
        if (target.width != level.width || target.height != level.height) return false;
        for (int by = 0; by < target.height; by += 4)
            for (int bx = 0; bx < target.width; bx += 4)
            {
                std::uint8_t* block = &texture[BlockOffset(target, bx, by)];
                Texel original[16];
                DecodeBlock(block, original);
                float colours[16][3];
                bool opaque[16];
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                    {
                        int i = y * 4 + x;
                        int sx = std::min(bx + x, target.width - 1), sy = std::min(by + y, target.height - 1);
                        for (int c = 0; c < 3; ++c) colours[i][c] = level.rgb[(std::size_t(sy) * level.width + sx) * 3 + c];
                        opaque[i] = original[i].alpha > 0.0f;
                    }
                EncodeBlock(colours, opaque, block);
            }
    }
    return true;
}
} // namespace

bool ValidRobotGoalieMap(std::span<const std::uint8_t> map)
{
    HostVector<Target> targets;
    return Parse(map, targets);
}

int MakeRobotGoalieTextures(std::span<const std::uint8_t> map, std::span<const std::uint8_t> robot,
    std::span<std::uint8_t> bundle)
{
    HostVector<Target> targets;
    if (!Parse(map, targets) || bundle.size() < 16) return 0;
    const Target* kit = nullptr;
    const Target* eye = nullptr;
    for (const Target& t : targets)
    {
        if (t.texture == kGoalieKitLayout) kit = &t;
        if (t.texture == kGoalieEye) eye = &t;
    }
    // Which textures the bundle has that become the robot's, before any work.
    const std::uint32_t count = BE32(&bundle[4]);
    if (count > (bundle.size() - 16) / 16) return 0;
    bool wantsKit = false, wantsEye = false, wantsMask = false;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint32_t texture = BE32(&bundle[16 + std::size_t(i) * 16]);
        wantsKit |= IsGoalieKit(texture);
        wantsEye |= texture == kGoalieEye;
        wantsMask |= texture == kGoalieShineMask;
    }
    if (!(wantsKit && kit) && !(wantsEye && eye) && !(wantsMask && kit)) return 0;

    std::span<const std::uint8_t> source = FindBundleTexture(robot, kRobotGoalieTexture);
    HostVector<Level> sourceLevels;
    std::uint32_t sourceFormat;
    if (source.empty() || !Levels(source, sourceFormat, sourceLevels) || sourceFormat != kCMPR) return 0;
    HostVector<Texel> texels;
    DecodeLevel(source, sourceFormat, sourceLevels[0], texels);

    Image kitImage, eyeImage, maskImage;
    if (kit && (wantsKit || wantsMask)) kitImage = Make(*kit, sourceLevels[0], texels);
    if (kit && wantsMask) maskImage = ShineMask(kitImage);
    if (eye && wantsEye) eyeImage = Make(*eye, sourceLevels[0], texels);

    int rewritten = 0;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        std::uint32_t texture = BE32(&bundle[16 + std::size_t(i) * 16]);
        const Image* image = IsGoalieKit(texture) && kit ? &kitImage
            : texture == kGoalieEye && eye ? &eyeImage
            : texture == kGoalieShineMask && kit ? &maskImage
            : nullptr;
        if (!image) continue;
        std::span<const std::uint8_t> found = FindBundleTexture(bundle, texture);
        if (found.empty()) continue;
        std::span<std::uint8_t> writable(const_cast<std::uint8_t*>(found.data()), found.size());
        rewritten += Write(writable, *image);
    }
    return rewritten;
}
} // namespace mscharged::platform
