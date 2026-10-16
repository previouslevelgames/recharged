// Mirror match away kits made from the home kit's textures
// (src/platform/away_kits.cpp): kit texels take the away colour, white trim
// the home colour, everything else keeps its bytes; team textures change only
// where they differ from the reference teams; bundles get the away texture
// under its new name.
#include "platform/away_kits.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace mscharged::platform;

extern "C" {
unsigned mscharged_tweak_away_kit_colour(const char* captain);
unsigned mscharged_tweak_away_kit_source(const char* captain);
const char* mscharged_tweak_away_kit_reference(const char* captain, int index);
bool mscharged_tweak_make_away_kit(const char* captain, bool team, void* bundle, unsigned long size, unsigned source,
    unsigned target, const void* reference0, unsigned long size0, unsigned texture0, const void* reference1,
    unsigned long size1, unsigned texture1);
}

namespace
{
unsigned checks;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

std::uint16_t To565(std::uint32_t rgb)
{
    unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    return std::uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v)
{
    out[at] = std::uint8_t(v >> 24);
    out[at + 1] = std::uint8_t(v >> 16);
    out[at + 2] = std::uint8_t(v >> 8);
    out[at + 3] = std::uint8_t(v);
}

// A 16x16 CMPR texture, one level, of solid 4x4 blocks: colours[by][bx].
std::vector<std::uint8_t> Texture(const std::uint32_t colours[4][4])
{
    std::vector<std::uint8_t> t(32 + 4 * 32, 0);
    Put32(t, 0, 1); // levels
    Put32(t, 4, 2); // CMPR
    t[14] = 0;
    t[15] = 16;
    t[16] = 0;
    t[17] = 16;
    for (int by = 0; by < 4; ++by)
        for (int bx = 0; bx < 4; ++bx)
        {
            int tile = (by / 2) * 2 + bx / 2, sub = (by & 1) * 2 + (bx & 1);
            std::uint8_t* block = &t[32 + tile * 32 + sub * 8];
            std::uint16_t c = To565(colours[by][bx]);
            block[0] = std::uint8_t(c >> 8);
            block[1] = std::uint8_t(c);
            block[2] = std::uint8_t(c >> 8);
            block[3] = std::uint8_t(c); // c0 == c1: three-colour mode, every index 0
        }
    return t;
}

const std::uint8_t* Block(const std::vector<std::uint8_t>& t, int bx, int by)
{
    int tile = (by / 2) * 2 + bx / 2, sub = (by & 1) * 2 + (bx & 1);
    return &t[32 + tile * 32 + sub * 8];
}

// The colour of a block's first texel.
void TexelColour(const std::uint8_t* block, float rgb[3])
{
    auto expand = [](std::uint16_t c, float* o) {
        o[0] = float(((c >> 11) & 31) * 255 / 31);
        o[1] = float(((c >> 5) & 63) * 255 / 63);
        o[2] = float((c & 31) * 255 / 31);
    };
    std::uint16_t c0 = std::uint16_t(block[0] << 8 | block[1]), c1 = std::uint16_t(block[2] << 8 | block[3]);
    float p0[3], p1[3];
    expand(c0, p0);
    expand(c1, p1);
    int index = block[4] >> 6;
    for (int k = 0; k < 3; ++k)
    {
        float v = index == 0 ? p0[k] : index == 1 ? p1[k] : c0 > c1 ? (index == 2 ? (2 * p0[k] + p1[k]) / 3 : (p0[k] + 2 * p1[k]) / 3) : (p0[k] + p1[k]) / 2;
        rgb[k] = v;
    }
}

std::uint32_t Hash(const char* s)
{
    std::uint32_t h = 0xFFFFFFFFu;
    while (*s) h = h + (h << 5) + static_cast<unsigned char>(*s++);
    return h;
}

// A bundle holding the given textures.
std::vector<std::uint8_t> Bundle(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& textures)
{
    std::vector<std::uint8_t> b(16 + 16 * textures.size(), 0);
    Put32(b, 4, std::uint32_t(textures.size()));
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        Put32(b, 16 + 16 * i, textures[i].first);
        Put32(b, 16 + 16 * i + 4, offset);
        Put32(b, 16 + 16 * i + 8, std::uint32_t(textures[i].second.size()));
        offset += std::uint32_t(textures[i].second.size());
    }
    for (const auto& t : textures) b.insert(b.end(), t.second.begin(), t.second.end());
    return b;
}

constexpr std::uint32_t kRed = 0xE00010, kWhite = 0xF8F8F8, kSkin = 0xF0B080, kGreen = 0x2A8A20,
                        kYellow = 0xF8C800, kPurple = 0x6018B0, kGrey = 0x808080;
} // namespace

int main()
{
    try
    {
        Check(FindGeneratedAwayKit("mario") && FindGeneratedAwayKit("luigi") && FindGeneratedAwayKit("wario")
                && FindGeneratedAwayKit("waluigi"),
            "The four captains without an away kit get one");
        Check(!FindGeneratedAwayKit("bowser") && !FindGeneratedAwayKit("peach") && !FindGeneratedAwayKit(nullptr),
            "Captains the game has an away kit for get none");
        Check(mscharged_tweak_away_kit_source("mario") == Hash("mario/mario"), "Mario's kit is mario/mario");
        Check(mscharged_tweak_away_kit_colour("bowser") == 0 && mscharged_tweak_away_kit_source("bowser") == 0
                && mscharged_tweak_away_kit_reference("bowser", 0) == nullptr,
            "No kit, no colour, texture or references");
        Check(mscharged_tweak_away_kit_colour("luigi") != 0 && mscharged_tweak_away_kit_reference("luigi", 0)
                && std::strcmp(mscharged_tweak_away_kit_reference("luigi", 0), "luigi") != 0,
            "A kit's references are other teams");

        // Mario: a red kit with a white detail in it and a patch of skin.
        const std::uint32_t mario[4][4] = {
            {kRed, kRed, kRed, kRed}, {kRed, kWhite, kRed, kRed}, {kRed, kRed, kRed, kRed}, {kRed, kRed, kRed, kSkin}};
        std::vector<std::uint8_t> kit = Texture(mario), before = kit;
        Check(RecolourAwayKit(*FindGeneratedAwayKit("mario"), AwayKitTexture::Kit, kit), "Mario's kit recolours");
        float c[3];
        TexelColour(Block(kit, 0, 0), c);
        Check(c[0] > 200 && c[1] > 200 && c[2] > 200, "Mario's red kit turns white");
        TexelColour(Block(kit, 1, 1), c);
        Check(c[0] > 150 && c[1] < 90 && c[2] < 90, "White trim in Mario's kit turns red");
        Check(std::memcmp(Block(kit, 3, 3), Block(before, 3, 3), 8) == 0, "Skin keeps its bytes");
        Check(std::memcmp(kit.data(), before.data(), 32) == 0, "The texture header is unchanged");

        // Luigi: green turns blue, other colours keep their bytes.
        const std::uint32_t luigi[4][4] = {
            {kGreen, kGreen, kGrey, kGrey}, {kGreen, kGreen, kGrey, kGrey}, {kSkin, kSkin, kGreen, kGreen},
            {kSkin, kSkin, kGreen, kGreen}};
        kit = Texture(luigi);
        before = kit;
        Check(RecolourAwayKit(*FindGeneratedAwayKit("luigi"), AwayKitTexture::Kit, kit), "Luigi's kit recolours");
        TexelColour(Block(kit, 0, 0), c);
        Check(c[2] > c[0] + 60 && c[2] > c[1], "Luigi's green kit turns blue");
        Check(std::memcmp(Block(kit, 2, 0), Block(before, 2, 0), 8) == 0
                && std::memcmp(Block(kit, 0, 2), Block(before, 0, 2), 8) == 0,
            "Grey and skin keep their bytes");

        // Wario's team texture: yellow where the references are green and
        // purple; grey everywhere is not team colour.
        const std::uint32_t wario[4][4] = {
            {kYellow, kYellow, kGrey, kGrey}, {kYellow, kYellow, kGrey, kGrey}, {kGrey, kGrey, kYellow, kYellow},
            {kGrey, kGrey, kYellow, kYellow}};
        std::uint32_t ref0[4][4], ref1[4][4], same[4][4];
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
            {
                ref0[y][x] = wario[y][x] == kYellow ? kGreen : kGrey;
                ref1[y][x] = wario[y][x] == kYellow ? kPurple : kGrey;
                same[y][x] = kYellow;
            }
        std::vector<std::uint8_t> team = Texture(wario), r0 = Texture(ref0), r1 = Texture(ref1);
        before = team;
        Check(RecolourAwayKit(*FindGeneratedAwayKit("wario"), AwayKitTexture::Team, team, r0, r1),
            "Wario's team texture recolours");
        TexelColour(Block(team, 0, 0), c);
        Check(c[2] > c[0] + 60, "Wario's yellow turns blue");
        Check(std::memcmp(Block(team, 2, 0), Block(before, 2, 0), 8) == 0, "Texels like the references keep their bytes");
        // A yellow texel that a reference also has is not the team's.
        std::vector<std::uint8_t> yellow = Texture(same), sameRef = Texture(same);
        before = yellow;
        Check(RecolourAwayKit(*FindGeneratedAwayKit("wario"), AwayKitTexture::Team, yellow, sameRef, r1),
            "A texture the references share recolours");
        Check(yellow == before, "Colours a reference team has too stay");
        Check(!RecolourAwayKit(*FindGeneratedAwayKit("wario"), AwayKitTexture::Team, team, {}, r1),
            "A team texture needs both references");

        // Bundles: the source texture becomes the away texture under its new
        // name; the other textures stay.
        const std::uint32_t source = Hash("mario/mario"), target = Hash("mario_alt/mario_alt"), other = Hash("other");
        std::vector<std::uint8_t> otherTexture = Texture(luigi);
        std::vector<std::uint8_t> bundle = Bundle({{other, otherTexture}, {source, Texture(mario)}});
        Check(mscharged_tweak_make_away_kit("mario", false, bundle.data(), bundle.size(), source, target, nullptr, 0, 0,
                  nullptr, 0, 0),
            "The bundle's kit becomes the away kit");
        Check(FindBundleTexture(bundle, source).empty() && !FindBundleTexture(bundle, target).empty(),
            "The away kit has the away name");
        auto kept = FindBundleTexture(bundle, other);
        Check(kept.size() == otherTexture.size() && std::memcmp(kept.data(), otherTexture.data(), kept.size()) == 0,
            "Other textures keep their bytes");
        TexelColour(&FindBundleTexture(bundle, target)[32], c);
        Check(c[0] > 200 && c[1] > 200 && c[2] > 200, "The renamed texture is the white kit");
        Check(!mscharged_tweak_make_away_kit("mario", false, bundle.data(), bundle.size(), source, target, nullptr, 0,
                  0, nullptr, 0, 0),
            "A bundle without the kit texture fails");
        Check(!mscharged_tweak_make_away_kit("bowser", false, bundle.data(), bundle.size(), target, source, nullptr, 0,
                  0, nullptr, 0, 0),
            "A captain without a made kit fails");

        // A texture the engine cannot recolour still gets the away name, so
        // the away team never looks for a missing texture.
        std::vector<std::uint8_t> rgb5a3 = Texture(mario);
        Put32(rgb5a3, 4, 1);
        bundle = Bundle({{source, rgb5a3}});
        Check(!mscharged_tweak_make_away_kit("mario", false, bundle.data(), bundle.size(), source, target, nullptr, 0, 0,
                  nullptr, 0, 0),
            "A texture other than CMPR is not recoloured");
        Check(!FindBundleTexture(bundle, target).empty(), "It still gets the away name");
        Check(FindBundleTexture(std::vector<std::uint8_t>(8, 0), source).empty(), "A truncated bundle has no textures");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "away_kits: %s (after %u checks)\n", error.what(), checks);
        return 1;
    }
    std::printf("away_kits: %u checks passed\n", checks);
    return 0;
}
