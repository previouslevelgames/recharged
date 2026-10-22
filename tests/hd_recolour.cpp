// Mirror match's away kits in HD (src/platform/hd_recolour.cpp): a texture
// pack's image of a recoloured texture is recoloured where the game's texture
// was, edges follow the HD image's colours, other pixels keep theirs, and
// images that do not fit (another shape, another picture, a grey kit) are
// left alone. DDS files hold every mip level.
#include "platform/hd_recolour.h"
#include "platform/kit_colour.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mscharged::platform;

namespace
{
unsigned checks;
void Check(bool value, const std::string& message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

struct Rgb
{
    float r, g, b;
};

constexpr std::uint8_t kRed[3] = {200, 30, 30};
constexpr std::uint8_t kDarkRed[3] = {110, 15, 15};
constexpr std::uint8_t kGreen[3] = {40, 170, 60};
constexpr std::uint8_t kSkin[3] = {235, 180, 140};

// The game's texture: 8x8, kit (red) in the left half, green on the right.
KitRecolour Game()
{
    KitRecolour kit;
    kit.width = kit.height = 8;
    kit.mask.assign(64, 0);
    kit.rgba.assign(64 * 4, 255);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            const std::uint8_t* c = x < 4 ? kRed : kGreen;
            for (int k = 0; k < 3; ++k) kit.rgba[(y * 8 + x) * 4 + k] = c[k];
            kit.mask[y * 8 + x] = x < 4;
        }
    const auto to = kit_colour::Target<Rgb>(0x2B5FE3u); // Mario's away blue
    kit.to[0] = to.L, kit.to[1] = to.C, kit.to[2] = to.h;
    return kit;
}

// The pack's 32x32 image of it: the kit shaded (dark rows), reaching one HD
// pixel past the game's edge, and a skin-coloured pixel in the kit's edge.
RgbaImage Pack()
{
    RgbaImage image;
    image.width = image.height = 32;
    image.rgba.assign(32 * 32 * 4, 255);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
        {
            const std::uint8_t* c = x < 17 ? (y % 8 == 0 ? kDarkRed : kRed) : kGreen;
            if (x == 15 && y == 5) c = kSkin;
            for (int k = 0; k < 3; ++k) image.rgba[(y * 32 + x) * 4 + k] = c[k];
        }
    return image;
}

const std::uint8_t* At(const RgbaImage& image, int x, int y) { return &image.rgba[(y * image.width + x) * 4]; }

// The game's texture and the pack's image scaled up by `scale` (big enough to
// be split between threads), with a gradient so the kit has many colours.
void Scaled(int scale, KitRecolour& kit, RgbaImage& image)
{
    const KitRecolour small = Game();
    kit = small;
    kit.width = kit.height = 8 * std::uint32_t(scale);
    kit.mask.assign(std::size_t(kit.width) * kit.height, 0);
    kit.rgba.assign(kit.mask.size() * 4, 255);
    for (std::uint32_t y = 0; y < kit.height; ++y)
        for (std::uint32_t x = 0; x < kit.width; ++x)
        {
            const std::size_t i = std::size_t(y) * kit.width + x, from = (y / scale) * 8 + x / scale;
            kit.mask[i] = small.mask[from];
            for (int k = 0; k < 3; ++k) kit.rgba[i * 4 + k] = small.rgba[from * 4 + k];
        }
    const RgbaImage pack = Pack();
    image.width = image.height = 32 * std::uint32_t(scale);
    image.rgba.assign(std::size_t(image.width) * image.height * 4, 255);
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::uint8_t* c = At(pack, int(x / scale), int(y / scale));
            std::uint8_t* out = &image.rgba[(std::size_t(y) * image.width + x) * 4];
            for (int k = 0; k < 3; ++k) out[k] = std::uint8_t(std::clamp(int(c[k]) + int((x + y) % 24) - 12, 0, 255));
        }
}

float HueDegrees(const std::uint8_t* p)
{
    const auto lab = kit_colour::ToLab(Rgb{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f});
    float h = std::atan2(lab.b, lab.a) * 180.0f / 3.14159265f;
    return h < 0 ? h + 360.0f : h;
}

bool Same(const std::uint8_t* p, const std::uint8_t* c) { return p[0] == c[0] && p[1] == c[1] && p[2] == c[2]; }
bool Blue(const std::uint8_t* p) { return p[2] > p[0] + 40 && p[2] > p[1] + 20; }
} // namespace

int main()
{
    try
    {
        const KitRecolour kit = Game();
        const float blueHue = HueDegrees(std::vector<std::uint8_t>{0x2B, 0x5F, 0xE3}.data());

        RgbaImage image = Pack();
        std::string why;
        Check(RecolourHdImage(kit, image, &why), "a fitting image is recoloured: " + why);
        // Kit pixels take the away colour, with their own shading.
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 12; ++x)
            {
                Check(Blue(At(image, x, y)), "kit pixel is blue");
                Check(std::fabs(HueDegrees(At(image, x, y)) - blueHue) < 6.0f, "kit pixel has the away hue");
            }
        Check(At(image, 3, 8)[2] < At(image, 3, 9)[2], "the kit's shading stays darker");
        // The kit's edge: HD kit pixels past the game's edge are kit, the skin pixel is not.
        Check(Blue(At(image, 16, 10)), "kit pixel past the game's kit edge is recoloured");
        Check(Same(At(image, 15, 5), kSkin), "a skin pixel at the kit's edge keeps its colour");
        // Other pixels keep their bytes.
        for (int y = 0; y < 32; ++y)
            for (int x = 17; x < 32; ++x) Check(Same(At(image, x, y), kGreen), "green pixel unchanged");
        Check(At(image, 0, 0)[3] == 255, "alpha unchanged");

        // Images that do not fit stay as they are.
        RgbaImage grey = Pack();
        for (std::size_t i = 0; i < grey.rgba.size(); i += 4)
        {
            const std::uint8_t v = std::uint8_t((grey.rgba[i] + grey.rgba[i + 1] + grey.rgba[i + 2]) / 3);
            grey.rgba[i] = grey.rgba[i + 1] = grey.rgba[i + 2] = v;
        }
        const RgbaImage greyBefore = grey;
        Check(!RecolourHdImage(kit, grey, &why) && grey.rgba == greyBefore.rgba, "a grey image is left alone");
        RgbaImage wide = Pack();
        wide.width = 64;
        wide.height = 16;
        Check(!RecolourHdImage(kit, wide, &why), "an image of another shape is left alone");
        RgbaImage other = Pack();
        for (std::size_t i = 0; i < other.rgba.size(); i += 4) std::copy(kRed, kRed + 3, other.rgba.begin() + std::ptrdiff_t(i));
        Check(!RecolourHdImage(kit, other, &why), "a picture with the kit's colour all over is left alone");
        RgbaImage teal = Pack();
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 17; ++x)
            {
                std::uint8_t* p = &teal.rgba[(y * 32 + x) * 4];
                p[0] = 30, p[1] = 160, p[2] = 200;
            }
        Check(!RecolourHdImage(kit, teal, &why), "a kit in another colour is left alone");
        // Drawn anew with another framing: the kit lies elsewhere.
        RgbaImage moved = Pack();
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
            {
                const std::uint8_t* c = x >= 8 && x < 26 ? kRed : kGreen;
                for (int k = 0; k < 3; ++k) moved.rgba[(y * 32 + x) * 4 + k] = c[k];
            }
        Check(!RecolourHdImage(kit, moved, &why), "a kit texture with the kit elsewhere is left alone");
        // Redrawn with the game's framing, a quarter of its kit a stripe in
        // another colour: recoloured, the stripe kept.
        constexpr std::uint8_t kViolet[3] = {120, 40, 190};
        RgbaImage striped = Pack();
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 4; ++x) std::copy(kViolet, kViolet + 3, striped.rgba.begin() + (y * 32 + x) * 4);
        why.clear();
        Check(RecolourHdImage(kit, striped, &why), "a redrawn kit with a stripe is recoloured: " + why);
        Check(Same(At(striped, 2, 20), kViolet) && Blue(At(striped, 10, 20)) && Same(At(striped, 25, 20), kGreen),
              "its stripe keeps its colour, the rest of its kit is blue");
        // A painted picture drawn anew: its kit is found by colour, skin and
        // other colours keep theirs.
        KitRecolour picture = kit;
        picture.picture = true;
        RgbaImage drawn = moved;
        std::copy(kSkin, kSkin + 3, drawn.rgba.begin() + (5 * 32 + 12) * 4);
        std::copy(kDarkRed, kDarkRed + 3, drawn.rgba.begin() + (9 * 32 + 12) * 4);
        why.clear();
        Check(RecolourHdImage(picture, drawn, &why) && why.find("by colour") != std::string::npos,
              "a picture drawn anew is recoloured by colour: " + why);
        Check(Blue(At(drawn, 20, 20)) && Blue(At(drawn, 8, 3)), "its kit, where it lies now, is blue");
        Check(Blue(At(drawn, 12, 9)), "its dark kit shading is blue");
        Check(Same(At(drawn, 12, 5), kSkin), "skin keeps its colour");
        Check(Same(At(drawn, 2, 2), kGreen) && Same(At(drawn, 30, 30), kGreen), "other colours keep theirs");
        RgbaImage greyPicture = grey;
        Check(!RecolourHdImage(picture, greyPicture, &why) && greyPicture.rgba == greyBefore.rgba,
              "a grey picture is left alone");
        RgbaImage same = Pack();
        KitRecolour none = kit;
        none.mask.assign(64, 0);
        Check(!RecolourHdImage(none, same, &why), "nothing to recolour");

        // On every core the result is the same as on one.
        for (const bool drawnAnew : {false, true})
        {
            KitRecolour big;
            RgbaImage one;
            Scaled(32, big, one);
            big.picture = drawnAnew;
            if (drawnAnew)
                for (std::uint32_t y = 0; y < one.height; ++y) // the kit moved right by a quarter
                    std::rotate(one.rgba.begin() + std::ptrdiff_t(y) * one.width * 4,
                                one.rgba.begin() + std::ptrdiff_t(y + 1) * one.width * 4 - one.width,
                                one.rgba.begin() + std::ptrdiff_t(y + 1) * one.width * 4);
            RgbaImage all = one;
            std::string whyOne, whyAll;
            const bool okOne = RecolourHdImage(big, one, &whyOne, 1), okAll = RecolourHdImage(big, all, &whyAll, 0);
            Check(okOne && okAll && (whyOne.find("by colour") != std::string::npos) == drawnAnew,
                  std::string(drawnAnew ? "a big picture drawn anew" : "a big image") + " is recoloured: " + whyOne);
            Check(one.rgba == all.rgba && whyOne == whyAll, "every core gives the same image as one");
        }

        // DDS: header, then every level.
        const auto dds = EncodeDds(image, true);
        Check(dds.size() == 128 + (32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1) * 4, "DDS holds the mip chain");
        Check(dds[0] == 'D' && dds[1] == 'D' && dds[2] == 'S' && dds[3] == ' ' && dds[28] == 6, "DDS header");
        Check(std::equal(image.rgba.begin(), image.rgba.end(), dds.begin() + 128), "DDS largest level");
        const auto single = EncodeDds(image, false);
        Check(single.size() == 128 + 32 * 32 * 4 && single[28] == 1, "DDS without mips");
        // An odd size: each smaller level averages 2x2 texels, the last row
        // or column again at the edge.
        RgbaImage odd;
        odd.width = 3, odd.height = 2;
        odd.rgba = {10, 0, 0, 255, 20, 0, 0, 255, 90, 0, 0, 255, 30, 0, 0, 255, 41, 0, 0, 255, 90, 0, 0, 255};
        const auto oddDds = EncodeDds(odd, true);
        Check(oddDds.size() == 128 + (6 + 1) * 4 && oddDds[128 + 24] == 25 && oddDds[128 + 27] == 255,
              "odd-sized mip level");
        RgbaImage tall;
        tall.width = 1, tall.height = 3;
        tall.rgba = {10, 0, 0, 255, 21, 0, 0, 255, 50, 0, 0, 255};
        const auto tallDds = EncodeDds(tall, true);
        Check(tallDds.size() == 128 + (3 + 1) * 4 && tallDds[128 + 12] == 16, "one texel wide mip level");
        std::printf("hd_recolour: %u checks passed\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "hd_recolour: FAILED after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
