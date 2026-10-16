// Mirror match away kits made from the home kit's textures and pictures
// (src/platform/away_kits.cpp): kit texels take the away colour, everything
// else keeps its bytes; team textures change only where they differ from the
// reference teams; pictures change only in areas, not single texels or thin
// lines; CMPR, RGB5A3 and RGBA8 textures; bundles get the away texture under
// its new name.
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
bool mscharged_tweak_recolour_away_texture(const char* captain, int kind, void* texture, unsigned long size,
    const void* reference0, unsigned long size0, const void* reference1, unsigned long size1);
bool mscharged_tweak_away_kit_photos(const char* captain);
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

// The same 16x16 picture as RGB5A3 (4x4 tiles, 2 bytes a texel) or RGBA8
// (4x4 tiles, 64 bytes: alpha-red pairs, then green-blue pairs).
std::vector<std::uint8_t> Texture16(const std::uint32_t colours[4][4], std::uint32_t format, unsigned alpha = 255)
{
    const std::size_t tileBytes = format == 3 ? 64 : 32;
    std::vector<std::uint8_t> t(32 + 16 * tileBytes, 0);
    Put32(t, 0, 1);
    Put32(t, 4, format);
    t[15] = 16;
    t[17] = 16;
    for (int tile = 0; tile < 16; ++tile)
    {
        std::uint32_t c = colours[tile / 4][tile % 4];
        unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        std::uint8_t* p = &t[32 + tile * tileBytes];
        for (int i = 0; i < 16; ++i)
        {
            if (format == 3)
            {
                p[2 * i] = std::uint8_t(alpha);
                p[2 * i + 1] = std::uint8_t(r);
                p[32 + 2 * i] = std::uint8_t(g);
                p[32 + 2 * i + 1] = std::uint8_t(b);
            }
            else
            {
                std::uint16_t v = alpha == 255
                    ? std::uint16_t(0x8000 | ((r * 31 + 127) / 255) << 10 | ((g * 31 + 127) / 255) << 5 | ((b * 31 + 127) / 255))
                    : std::uint16_t((alpha * 7 / 255) << 12 | ((r * 15 + 127) / 255) << 8 | ((g * 15 + 127) / 255) << 4
                          | ((b * 15 + 127) / 255));
                p[2 * i] = std::uint8_t(v >> 8);
                p[2 * i + 1] = std::uint8_t(v);
            }
        }
    }
    return t;
}

// A 16x16 RGBA8 picture, one level, texel by texel.
template<class Colour> std::vector<std::uint8_t> Picture(Colour colour)
{
    std::vector<std::uint8_t> t(32 + 16 * 64, 0);
    Put32(t, 0, 1);
    Put32(t, 4, 3);
    t[15] = 16;
    t[17] = 16;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            std::uint32_t c = colour(x, y);
            std::uint8_t* p = &t[32 + ((y / 4) * 4 + x / 4) * 64];
            int i = (y % 4) * 4 + x % 4;
            p[2 * i] = 255;
            p[2 * i + 1] = std::uint8_t(c >> 16);
            p[32 + 2 * i] = std::uint8_t(c >> 8);
            p[32 + 2 * i + 1] = std::uint8_t(c);
        }
    return t;
}

// A texel of such a picture: r, g, b.
void PictureTexel(const std::vector<std::uint8_t>& t, int x, int y, float rgb[3])
{
    const std::uint8_t* p = &t[32 + ((y / 4) * 4 + x / 4) * 64];
    int i = (y % 4) * 4 + x % 4;
    rgb[0] = p[2 * i + 1];
    rgb[1] = p[32 + 2 * i];
    rgb[2] = p[32 + 2 * i + 1];
}

// The first texel of a 16x16 RGB5A3 or RGBA8 texture's tile: r, g, b (0..255) and alpha.
void TileColour(const std::vector<std::uint8_t>& t, std::uint32_t format, int tile, float rgb[3], unsigned& alpha)
{
    if (format == 3)
    {
        const std::uint8_t* p = &t[32 + tile * 64];
        alpha = p[0];
        rgb[0] = p[1];
        rgb[1] = p[32];
        rgb[2] = p[33];
        return;
    }
    std::uint16_t v = std::uint16_t(t[32 + tile * 32] << 8 | t[32 + tile * 32 + 1]);
    if (v & 0x8000)
    {
        alpha = 255;
        rgb[0] = float(((v >> 10) & 31) * 255 / 31);
        rgb[1] = float(((v >> 5) & 31) * 255 / 31);
        rgb[2] = float((v & 31) * 255 / 31);
    }
    else
    {
        alpha = (v >> 12) & 7;
        rgb[0] = float(((v >> 8) & 15) * 17);
        rgb[1] = float(((v >> 4) & 15) * 17);
        rgb[2] = float((v & 15) * 17);
    }
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
                        kYellow = 0xF8C800, kPurple = 0x6018B0, kGrey = 0x808080, kHair = 0x8A3A1E, kPink = 0xE85A9A,
                        kLips = 0xD83A5A;
} // namespace

int main()
{
    try
    {
        Check(FindGeneratedAwayKit("mario") && FindGeneratedAwayKit("luigi") && FindGeneratedAwayKit("wario")
                && FindGeneratedAwayKit("waluigi"),
            "The four captains without an away kit get one");
        Check(!FindGeneratedAwayKit("bowser") && !FindGeneratedAwayKit("daisy") && !FindGeneratedAwayKit(nullptr),
            "Captains the game has a whole away kit for get none");
        Check(FindGeneratedAwayKit("peach") && mscharged_tweak_away_kit_source("peach") == 0
                && mscharged_tweak_away_kit_colour("peach") == 0x5DB9FF,
            "Peach's pictures are made in her blue kit's colour, her field kit is the game's");
        Check(mscharged_tweak_away_kit_source("mario") == Hash("mario/mario"), "Mario's kit is mario/mario");
        Check(mscharged_tweak_away_kit_colour("bowser") == 0 && mscharged_tweak_away_kit_source("bowser") == 0
                && mscharged_tweak_away_kit_reference("bowser", 0) == nullptr,
            "No kit, no colour, texture or references");
        Check(mscharged_tweak_away_kit_colour("luigi") != 0 && mscharged_tweak_away_kit_reference("luigi", 0)
                && std::strcmp(mscharged_tweak_away_kit_reference("luigi", 0), "luigi") != 0,
            "A kit's references are other teams");

        // Mario: a red kit with a white detail in it, a patch of skin and
        // reddish brown hair next to it.
        const std::uint32_t mario[4][4] = {
            {kRed, kRed, kRed, kRed}, {kRed, kWhite, kRed, kRed}, {kRed, kRed, kRed, kHair}, {kRed, kRed, kRed, kSkin}};
        std::vector<std::uint8_t> kit = Texture(mario), before = kit;
        Check(RecolourAwayKit(*FindGeneratedAwayKit("mario"), AwayKitTexture::Kit, kit), "Mario's kit recolours");
        float c[3];
        TexelColour(Block(kit, 0, 0), c);
        Check(c[2] > c[0] + 80 && c[2] > c[1] + 40, "Mario's red kit turns blue");
        Check(std::memcmp(Block(kit, 1, 1), Block(before, 1, 1), 8) == 0, "White details in the kit stay white");
        Check(std::memcmp(Block(kit, 3, 3), Block(before, 3, 3), 8) == 0, "Skin keeps its bytes");
        Check(std::memcmp(Block(kit, 3, 2), Block(before, 3, 2), 8) == 0, "Hair next to the kit keeps its bytes");
        Check(std::memcmp(kit.data(), before.data(), 32) == 0, "The texture header is unchanged");

        // Peach's pictures: her pink dress turns blue, her lips stay.
        const std::uint32_t peach[4][4] = {
            {kPink, kPink, kPink, kPink}, {kPink, kPink, kLips, kPink}, {kPink, kPink, kPink, kPink}, {kSkin, kSkin, kSkin, kSkin}};
        std::vector<std::uint8_t> dress = Texture(peach);
        before = dress;
        Check(RecolourAwayKit(*FindGeneratedAwayKit("peach"), AwayKitTexture::Kit, dress), "Peach's picture recolours");
        TexelColour(Block(dress, 0, 0), c);
        Check(c[2] > c[0] + 60, "Peach's pink turns blue");
        Check(std::memcmp(Block(dress, 2, 1), Block(before, 2, 1), 8) == 0, "Her lips keep their bytes");

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
        Check(c[2] > c[0] + 80, "The renamed texture is the blue kit");
        Check(!mscharged_tweak_make_away_kit("mario", false, bundle.data(), bundle.size(), source, target, nullptr, 0,
                  0, nullptr, 0, 0),
            "A bundle without the kit texture fails");
        Check(!mscharged_tweak_make_away_kit("bowser", false, bundle.data(), bundle.size(), target, source, nullptr, 0,
                  0, nullptr, 0, 0),
            "A captain without a made kit fails");

        // RGB5A3 and RGBA8 pictures: kit texels recolour and keep their
        // alpha, other texels keep their bytes.
        for (std::uint32_t format : {1u, 3u})
            for (unsigned alpha : {255u, 160u})
            {
                std::vector<std::uint8_t> picture = Texture16(mario, format, alpha);
                before = picture;
                Check(mscharged_tweak_recolour_away_texture("mario", 0, picture.data(), picture.size(), nullptr, 0,
                          nullptr, 0),
                    "An RGB5A3 or RGBA8 texture recolours");
                unsigned a = 0, b = 0;
                float o[3];
                TileColour(picture, format, 0, c, a);
                TileColour(before, format, 0, o, b);
                Check(c[2] > c[0] + 60 && a == b, "Its red turns blue and keeps its alpha");
                Check(std::memcmp(&picture[32 + 15 * (format == 3 ? 64 : 32)],
                          &before[32 + 15 * (format == 3 ? 64 : 32)], format == 3 ? 64 : 32)
                        == 0,
                    "Its skin keeps its bytes");
            }
        // Team pictures through the hooks' interface: Wario's yellow, where
        // the references differ, turns blue.
        std::vector<std::uint8_t> sidekick = Texture16(wario, 1), s0 = Texture16(ref0, 1), s1 = Texture16(ref1, 1);
        before = sidekick;
        Check(mscharged_tweak_recolour_away_texture("wario", 1, sidekick.data(), sidekick.size(), s0.data(),
                  s0.size(), s1.data(), s1.size()),
            "A team picture recolours");
        unsigned a = 0;
        TileColour(sidekick, 1, 0, c, a);
        Check(c[2] > c[0] + 60, "Its team colour turns blue");
        Check(std::memcmp(&sidekick[32 + 2 * 32], &before[32 + 2 * 32], 32) == 0, "Its other texels keep their bytes");
        Check(!mscharged_tweak_recolour_away_texture("wario", 1, sidekick.data(), sidekick.size(), nullptr, 0,
                  s1.data(), s1.size()),
            "A team picture needs both references");
        Check(!mscharged_tweak_recolour_away_texture("bowser", 0, sidekick.data(), sidekick.size(), nullptr, 0,
                  nullptr, 0),
            "A captain without a made kit recolours nothing");
        Check(!mscharged_tweak_recolour_away_texture("mario", 3, sidekick.data(), sidekick.size(), nullptr, 0,
                  nullptr, 0),
            "An unknown kind recolours nothing");

        // Painted pictures: the cap is an area of red; a thin line and a
        // single texel of red shading in the skin stay, unlike in a kit texture.
        auto painting = [&](int x, int y) {
            if (x < 10 && y < 10) return kRed;
            if (x == 13 || (x == 5 && y == 13)) return kRed;
            return kSkin;
        };
        std::vector<std::uint8_t> picture = Picture(painting), kitPicture = picture;
        Check(mscharged_tweak_recolour_away_texture("mario", 2, picture.data(), picture.size(), nullptr, 0, nullptr, 0),
            "A picture recolours");
        Check(mscharged_tweak_recolour_away_texture("mario", 0, kitPicture.data(), kitPicture.size(), nullptr, 0,
                  nullptr, 0),
            "The same picture as a kit texture recolours");
        PictureTexel(picture, 0, 0, c);
        Check(c[2] > c[0] + 80, "The picture's cap turns blue to its corner");
        PictureTexel(picture, 9, 9, c);
        Check(c[2] > c[0] + 80, "and to its edge");
        PictureTexel(picture, 13, 4, c);
        Check(c[0] > 200 && c[2] < 60, "A thin line stays red in a picture");
        PictureTexel(picture, 5, 13, c);
        Check(c[0] > 200 && c[2] < 60, "A single texel stays red in a picture");
        PictureTexel(kitPicture, 13, 4, c);
        Check(c[2] > c[0] + 80, "A kit texture's thin line turns blue");
        Check(mscharged_tweak_away_kit_photos("mario") && !mscharged_tweak_away_kit_photos("luigi")
                && !mscharged_tweak_away_kit_photos("bowser") && !mscharged_tweak_away_kit_photos(nullptr),
            "Only Mario's photos recolour");

        // A texture the engine cannot recolour still gets the away name, so
        // the away team never looks for a missing texture.
        std::vector<std::uint8_t> paletted = Texture(mario);
        Put32(paletted, 4, 8);
        bundle = Bundle({{source, paletted}});
        Check(!mscharged_tweak_make_away_kit("mario", false, bundle.data(), bundle.size(), source, target, nullptr, 0, 0,
                  nullptr, 0, 0),
            "A paletted texture is not recoloured");
        Check(!FindBundleTexture(bundle, target).empty(), "It still gets the away name");
        std::vector<std::uint8_t> truncated = Texture16(mario, 1);
        truncated.resize(truncated.size() - 1);
        Check(!mscharged_tweak_recolour_away_texture("mario", 0, truncated.data(), truncated.size(), nullptr, 0,
                  nullptr, 0),
            "A texture shorter than its levels is not recoloured");
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
