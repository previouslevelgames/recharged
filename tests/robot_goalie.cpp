// Robot goalies (src/platform/robot_goalie.cpp): the map is validated whole;
// every goalie texture of a bundle (teams' kits and away kits, eye, shine mask)
// becomes the robot's in place, every level, keeping transparent texels; other
// textures, textures of another size and bundles without the robot texture stay
// unchanged. Also checks the shipped map (path as the first argument).
#include "platform/gx_texture.h"
#include "platform/robot_goalie.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

using namespace mscharged::platform;

namespace
{
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

using Bytes = std::vector<std::uint8_t>;

std::uint32_t NameHash(const char* name)
{
    std::uint32_t hash = 0xFFFFFFFFu;
    for (; *name; ++name) hash = hash + (hash << 5) + std::uint8_t(*name);
    return hash;
}

void PutBE(Bytes& out, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out[at + i] = std::uint8_t(v >> (24 - 8 * i));
}
void AppendLE32(Bytes& out, std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i))); }
void AppendLE16(Bytes& out, std::uint16_t v) { out.push_back(std::uint8_t(v)); out.push_back(std::uint8_t(v >> 8)); }

// A CMPR texture as bundles store it: `levels` levels of a size x size picture,
// colour(x, y) on level 0 (smaller levels solid grey). `hole` makes texel (0, 0)
// of level 0 transparent.
template<class Colour> Bytes Cmpr(int size, int levels, Colour colour, bool hole = false)
{
    std::size_t bytes = 32;
    for (int l = 0, s = size; l < levels; ++l, s = std::max(s / 2, 1)) bytes += std::size_t((s + 7) / 8) * ((s + 7) / 8) * 32;
    Bytes t(bytes, 0);
    PutBE(t, 0, std::uint32_t(levels));
    PutBE(t, 4, gx::kCMPR);
    t[14] = std::uint8_t(size >> 8); t[15] = std::uint8_t(size);
    t[16] = std::uint8_t(size >> 8); t[17] = std::uint8_t(size);
    gx::HostVector<gx::Level> layout;
    std::uint32_t format;
    if (!gx::Levels(t, format, layout)) throw std::runtime_error("bad synthetic texture");
    for (std::size_t l = 0; l < layout.size(); ++l)
        for (int by = 0; by < layout[l].height; by += 4)
            for (int bx = 0; bx < layout[l].width; bx += 4)
            {
                float colours[16][3];
                bool opaque[16];
                for (int i = 0; i < 16; ++i)
                {
                    std::uint32_t c = l ? 0x808080u : colour(bx + i % 4, by + i / 4);
                    colours[i][0] = float(c >> 16 & 255); colours[i][1] = float(c >> 8 & 255); colours[i][2] = float(c & 255);
                    opaque[i] = !(hole && l == 0 && bx == 0 && by == 0 && i == 0);
                }
                gx::EncodeBlock(colours, opaque, &t[gx::BlockOffset(layout[l], bx, by)]);
            }
    return t;
}

// A texture bundle (.rlt) of these named textures.
Bytes Bundle(const std::vector<std::pair<std::uint32_t, Bytes>>& textures)
{
    Bytes out(16 + 16 * textures.size(), 0);
    std::memcpy(out.data(), "PTLG", 4);
    PutBE(out, 4, std::uint32_t(textures.size()));
    std::size_t offset = 0;
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        PutBE(out, 16 + 16 * i, textures[i].first);
        PutBE(out, 20 + 16 * i, std::uint32_t(offset));
        PutBE(out, 24 + 16 * i, std::uint32_t(textures[i].second.size()));
        offset += textures[i].second.size();
    }
    for (auto& [hash, texture] : textures) out.insert(out.end(), texture.begin(), texture.end());
    return out;
}

struct TargetSpec
{
    std::uint32_t texture;
    int width, height;
    float s, t; // every texel samples this point
    int samples = 1;
};

Bytes Map(const std::vector<TargetSpec>& targets)
{
    Bytes out = {'R', 'G', 'M', '1'};
    AppendLE32(out, 1);
    AppendLE32(out, kRobotGoalieTexture);
    AppendLE32(out, std::uint32_t(targets.size()));
    for (const TargetSpec& t : targets)
    {
        const int texels = t.width * t.height;
        AppendLE32(out, t.texture);
        AppendLE16(out, std::uint16_t(t.width));
        AppendLE16(out, std::uint16_t(t.height));
        AppendLE32(out, std::uint32_t(texels * t.samples));
        for (int k = 0; k < texels; ++k) out.push_back(std::uint8_t(t.samples));
        for (int k = 0; k < texels * t.samples; ++k)
        {
            AppendLE16(out, std::uint16_t(std::lround(t.s * 65535)));
            AppendLE16(out, std::uint16_t(std::lround(t.t * 65535)));
        }
    }
    return out;
}

// The colour (0..255) of texel (x, y) of level `level` of a bundle's texture.
void Texel(const Bytes& bundle, std::uint32_t hash, std::size_t level, int x, int y, float out[4])
{
    auto texture = FindBundleTexture(bundle, hash);
    gx::HostVector<gx::Level> levels;
    std::uint32_t format;
    if (texture.empty() || !gx::Levels(texture, format, levels) || level >= levels.size()) throw std::runtime_error("no texel");
    gx::HostVector<gx::Texel> texels;
    gx::DecodeLevel(texture, format, levels[level], texels);
    const gx::Texel& t = texels[std::size_t(y) * levels[level].width + x];
    out[0] = t.colour.r * 255; out[1] = t.colour.g * 255; out[2] = t.colour.b * 255; out[3] = t.alpha;
}

bool Near(const float a[4], std::uint32_t rgb, float tolerance = 12)
{
    return std::fabs(a[0] - float(rgb >> 16 & 255)) <= tolerance && std::fabs(a[1] - float(rgb >> 8 & 255)) <= tolerance
        && std::fabs(a[2] - float(rgb & 255)) <= tolerance;
}

// The robot texture: left half red, right half blue.
Bytes Robot()
{
    return Bundle({{kRobotGoalieTexture, Cmpr(16, 1, [](int x, int) { return x < 8 ? 0xFF0000u : 0x0000FFu; })},
        {0x12345678u, Cmpr(8, 1, [](int, int) { return 0x00FF00u; })}});
}

void MapValidation()
{
    const Bytes good = Map({{kGoalieKitLayout, 8, 8, 0.25f, 0.5f}, {kGoalieEye, 8, 4, 0.75f, 0.5f, 3}});
    Check(ValidRobotGoalieMap(good), "A valid map was refused");
    for (std::size_t n = 0; n < good.size(); ++n)
        Check(!ValidRobotGoalieMap(std::span(good.data(), n)), "A truncated map was accepted");
    Bytes trailing = good;
    trailing.push_back(0);
    Check(!ValidRobotGoalieMap(trailing), "A map with trailing bytes was accepted");
    // Header and table fields: every value but the right one is refused.
    for (std::size_t at : {0u, 1u, 2u, 3u, 4u, 8u, 9u, 12u})
    {
        Bytes bad = good;
        bad[at] ^= 0x40;
        Check(!ValidRobotGoalieMap(bad), "A map with a wrong header was accepted");
    }
    Bytes zeroCount = good;
    zeroCount[16 + 12] = 0; // first texel: no sample
    Check(!ValidRobotGoalieMap(zeroCount), "A texel without samples was accepted");
    Bytes fiveSamples = good;
    fiveSamples[16 + 12] = 5;
    Check(!ValidRobotGoalieMap(fiveSamples), "A texel with five samples was accepted");
    Bytes wrongTotal = good;
    wrongTotal[16 + 8] ^= 1;
    Check(!ValidRobotGoalieMap(wrongTotal), "A map whose sample total disagrees was accepted");
    Bytes zeroWidth = good;
    zeroWidth[16 + 4] = zeroWidth[16 + 5] = 0;
    Check(!ValidRobotGoalieMap(zeroWidth), "A target without texels was accepted");
    Check(!ValidRobotGoalieMap(Map({{kGoalieEye, 4, 4, 0, 0}, {kGoalieEye, 4, 4, 0, 0}})), "Duplicate targets were accepted");
    Check(!ValidRobotGoalieMap(Map({})), "A map without targets was accepted");
}

void Rewrite()
{
    const Bytes robot = Robot();
    const Bytes map = Map({{kGoalieKitLayout, 16, 16, 0.25f, 0.5f}, {kGoalieEye, 8, 8, 0.75f, 0.5f}});
    const std::uint32_t luigi = NameHash("luigigoalie/luigigoalie"), petey = NameHash("peteygoalie_alt/peteygoalie_alt");
    const std::uint32_t other = NameHash("luigigoalie/extra");
    auto green = [](int, int) { return 0x30A030u; };
    Bytes bundle = Bundle({{luigi, Cmpr(16, 2, green, true)}, {petey, Cmpr(16, 1, green)}, {other, Cmpr(16, 1, green)},
        {kGoalieEye, Cmpr(8, 1, green)}, {kGoalieShineMask, Cmpr(16, 1, green)},
        {NameHash("mariogoalie/mariogoalie"), Cmpr(8, 1, green)}});
    const Bytes before = bundle;
    const int rewritten = MakeRobotGoalieTextures(map, robot, bundle);
    Check(rewritten == 4, "Not every goalie texture of the right size was rewritten");
    float c[4];
    for (int y = 0; y < 16; y += 5)
        for (int x = 0; x < 16; x += 5)
        {
            Texel(bundle, luigi, 0, x, y, c);
            Check((x == 0 && y == 0) ? c[3] == 0 : Near(c, 0xFF0000u), "A team kit is not the robot's colour");
            Texel(bundle, petey, 0, x, y, c);
            Check(Near(c, 0xFF0000u), "An away kit is not the robot's colour");
        }
    Texel(bundle, luigi, 1, 3, 3, c);
    Check(Near(c, 0xFF0000u), "A smaller level of the kit kept its old picture");
    Texel(bundle, kGoalieEye, 0, 4, 4, c);
    Check(Near(c, 0x0000FFu), "The eye is not the robot's");
    Texel(bundle, kGoalieShineMask, 0, 4, 4, c);
    const float luma = 0.2126f, shine = std::fmin(0.3f + 1.2f * luma, 1.0f) * 255;
    Check(std::fabs(c[0] - shine) < 12 && std::fabs(c[1] - c[0]) < 4 && std::fabs(c[2] - c[0]) < 4,
        "The shine mask does not follow the made kit");
    auto same = [&](std::uint32_t hash) {
        auto a = FindBundleTexture(before, hash), b = FindBundleTexture(bundle, hash);
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
    };
    Check(same(other), "A texture that is no goalie texture changed");
    Check(same(NameHash("mariogoalie/mariogoalie")), "A kit of another size than the map's changed");
    // The directory never changes, and the same rewrite again gives the same bytes.
    Check(std::memcmp(before.data(), bundle.data(), 16 + 16 * 6) == 0, "The bundle's directory changed");
    Bytes again = bundle;
    Check(MakeRobotGoalieTextures(map, robot, again) == 4 && again == bundle, "Rewriting again changed the result");
}

void Refusals()
{
    const Bytes robot = Robot();
    const Bytes map = Map({{kGoalieKitLayout, 16, 16, 0.25f, 0.5f}});
    auto green = [](int, int) { return 0x30A030u; };
    const Bytes kit = Bundle({{NameHash("daisygoalie/daisygoalie"), Cmpr(16, 1, green)}});
    Bytes bundle = kit;
    Check(MakeRobotGoalieTextures(Bytes{}, robot, bundle) == 0 && bundle == kit, "An empty map rewrote textures");
    Check(MakeRobotGoalieTextures(map, Bundle({{0x12345678u, Cmpr(8, 1, green)}}), bundle) == 0 && bundle == kit,
        "A bundle without the robot texture rewrote textures");
    Check(MakeRobotGoalieTextures(map, Bytes(10, 0), bundle) == 0 && bundle == kit, "A truncated robot bundle rewrote textures");
    const Bytes none = Bundle({{NameHash("daisy/daisy"), Cmpr(16, 1, green)}});
    bundle = none;
    Check(MakeRobotGoalieTextures(map, robot, bundle) == 0 && bundle == none, "A bundle without goalie textures changed");
    Bytes broken = kit;
    broken.at(6) = 0x03; // 1000 textures, more than the bundle holds
    broken.at(7) = 0xE8;
    const Bytes brokenBefore = broken;
    Check(MakeRobotGoalieTextures(map, robot, broken) == 0 && broken == brokenBefore, "A broken bundle was rewritten");
    // An eye without an eye target and a mask without a kit target stay.
    const Bytes eyeOnly = Bundle({{kGoalieEye, Cmpr(8, 1, green)}});
    bundle = eyeOnly;
    Check(MakeRobotGoalieTextures(Map({{kGoalieEye, 4, 4, 0.5f, 0.5f}}), robot, bundle) == 0 && bundle == eyeOnly,
        "An eye of another size than the map's changed");
}

void ShippedMap(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    Check(bool(file), "The shipped map cannot be read");
    const Bytes map((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Check(ValidRobotGoalieMap(map), "The shipped map is not valid");
    // Its targets: Charged's goalie kit layout (256x256) and eye (128x64).
    Check(map.size() > 16 && map[12] == 2, "The shipped map does not have two targets");
    std::size_t at = 16;
    for (int i = 0; i < 2; ++i)
    {
        std::uint32_t texture = map[at] | map[at + 1] << 8 | map[at + 2] << 16 | std::uint32_t(map[at + 3]) << 24;
        int width = map[at + 4] | map[at + 5] << 8, height = map[at + 6] | map[at + 7] << 8;
        std::uint32_t samples = map[at + 8] | map[at + 9] << 8 | map[at + 10] << 16 | std::uint32_t(map[at + 11]) << 24;
        Check(i == 0 ? texture == kGoalieKitLayout && width == 256 && height == 256
                     : texture == kGoalieEye && width == 128 && height == 64,
            "The shipped map's targets are not the goalie kit and eye");
        at += 12 + std::size_t(width) * height + std::size_t(samples) * 4;
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        MapValidation();
        Rewrite();
        Refusals();
        if (argc > 1) ShippedMap(argv[1]);
        std::printf("robot goalie checks: %u passed\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "robot goalie test failed after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
