// Mii heads (mii_head.h): the U8 archive and parts file readers reject
// damaged bytes, record fields sit where the Wii's Mii library reads them,
// and synthetic parts draw the pixels the GX steps give. With
// MSCHARGED_TEST_DISC (a Charged image) the real parts draw every part, and
// with MSCHARGED_TEST_MII_REFERENCE as well (a folder of the game's own
// icons, see TestGameIcons) the heads are checked against the game's.
#include "platform/mii_head.h"
#include "platform/mii_maker.h"
#include "platform/rfl_character_transport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace mscharged::platform;
using namespace mscharged::platform::mii;
using Bytes = std::vector<std::uint8_t>;

unsigned checks{};
void Check(bool value, const std::string& message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}

template <class F> void Rejects(F&& f, const std::string& message)
{
    ++checks;
    try {
        f();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("accepted: " + message);
}

void Put16(Bytes& b, unsigned v) { b.push_back(std::uint8_t(v >> 8)), b.push_back(std::uint8_t(v)); }
void Put32(Bytes& b, std::uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8) b.push_back(std::uint8_t(v >> s));
}
void Set32(Bytes& b, std::size_t at, std::uint32_t v)
{
    for (int n = 0; n < 4; ++n) b[at + std::size_t(n)] = std::uint8_t(v >> (24 - 8 * n));
}

// ---------------------------------------------------------------- U8

// A U8 archive: root, directory "d" holding "f.bin", then "top.dat".
Bytes MakeU8(const Bytes& inner, const Bytes& top)
{
    const std::string names = std::string("\0d\0f.bin\0top.dat\0", 17);
    const std::uint32_t nodes = 0x20, header = 4 * 12 + std::uint32_t(names.size());
    const std::uint32_t data = (nodes + header + 31) & ~31u;
    Bytes b;
    Put32(b, 0x55AA382D), Put32(b, nodes), Put32(b, header), Put32(b, data);
    b.resize(nodes, 0);
    const auto node = [&](int type, std::uint32_t name, std::uint32_t offset, std::uint32_t size) {
        Put32(b, (std::uint32_t(type) << 24) | name), Put32(b, offset), Put32(b, size);
    };
    node(1, 0, 0, 4);
    node(1, 1, 0, 3);
    node(0, 3, data, std::uint32_t(inner.size()));
    node(0, 9, data + std::uint32_t(inner.size()), std::uint32_t(top.size()));
    b.insert(b.end(), names.begin(), names.end());
    b.resize(data, 0);
    b.insert(b.end(), inner.begin(), inner.end());
    b.insert(b.end(), top.begin(), top.end());
    return b;
}

void TestU8()
{
    const Bytes inner = {1, 2, 3}, top = {9, 8, 7, 6};
    const Bytes arc = MakeU8(inner, top);
    Check(ReadU8File(arc, "d/f.bin") == inner, "U8 file in a directory");
    Check(ReadU8File(arc, "/top.dat") == top, "U8 file at the root, leading slash");
    Check(!ReadU8File(arc, "f.bin"), "U8 file outside its directory");
    Check(!ReadU8File(arc, "d"), "U8 directory is not a file");
    Check(!ReadU8File(arc, "missing"), "U8 missing file");

    for (std::size_t cut = 0; cut < arc.size(); ++cut) {
        const Bytes shorter(arc.begin(), arc.begin() + std::ptrdiff_t(cut));
        Rejects([&] { ReadU8File(shorter, "top.dat"); }, "truncated U8 archive at " + std::to_string(cut));
    }
    Bytes bad = arc;
    bad[0] = 0;
    Rejects([&] { ReadU8File(bad, "top.dat"); }, "U8 magic");
    bad = arc;
    Set32(bad, 0x20 + 8, 1000000); // root: node count
    Rejects([&] { ReadU8File(bad, "top.dat"); }, "U8 node count");
    bad = arc;
    Set32(bad, 0x20 + 3 * 12, 0x00FFFFFF); // "f.bin": name offset
    Rejects([&] { ReadU8File(bad, "top.dat"); }, "U8 name offset");
    bad = arc;
    Set32(bad, 0x20 + 3 * 12 + 8, 0x7FFFFFFF); // "top.dat": size
    Rejects([&] { ReadU8File(bad, "top.dat"); }, "U8 file size");
    bad = arc;
    Set32(bad, 0x20 + 12 + 8, 1); // "d": ends before itself
    Rejects([&] { ReadU8File(bad, "top.dat"); }, "U8 directory end");
    std::mt19937 random(7);
    for (int n = 0; n < 200; ++n) {
        Bytes garbage(std::size_t(random() % 512));
        for (auto& v : garbage) v = std::uint8_t(random());
        Rejects([&] { ReadU8File(garbage, "x"); }, "garbage U8 archive");
    }
}

// ---------------------------------------------------------------- parts

using Archives = std::array<std::vector<Bytes>, 18>;

Bytes MakeParts(const Archives& archives)
{
    Bytes b;
    Put16(b, 18), Put16(b, 0x39D);
    b.resize(4 + 4 * 18, 0);
    for (std::size_t a = 0; a < 18; ++a) {
        Set32(b, 4 + 4 * a, std::uint32_t(b.size()));
        std::size_t biggest = 0;
        for (const auto& f : archives[a]) biggest = std::max(biggest, f.size());
        Put16(b, unsigned(archives[a].size())), Put16(b, unsigned(biggest));
        std::uint32_t offset = 0;
        for (const auto& f : archives[a]) Put32(b, offset), offset += std::uint32_t(f.size());
        Put32(b, offset);
        for (const auto& f : archives[a]) b.insert(b.end(), f.begin(), f.end());
    }
    return b;
}

void TestParts()
{
    Archives archives;
    for (std::size_t a = 0; a < 18; ++a) archives[a] = {Bytes{std::uint8_t(a)}, Bytes(), Bytes{1, 2, std::uint8_t(a)}};
    const Bytes bytes = MakeParts(archives);
    const MiiParts parts(bytes);
    for (int a = 0; a < 18; ++a) {
        Check(parts.FileCount(a) == 3, "parts file count");
        const auto f0 = parts.File(a, 0), f1 = parts.File(a, 1), f2 = parts.File(a, 2);
        Check(f0.size == 1 && f0.data[0] == a, "parts first file");
        Check(f1.size == 0, "parts empty file");
        Check(f2.size == 3 && f2.data[2] == a, "parts third file");
        Check(parts.File(a, 3).size == 0 && parts.File(a, -1).size == 0, "parts file past the end");
    }
    Check(parts.File(18, 0).size == 0 && parts.FileCount(-1) == 0, "parts archive past the end");

    for (std::size_t cut = 0; cut < bytes.size(); ++cut)
        Rejects([&] { MiiParts(Bytes(bytes.begin(), bytes.begin() + std::ptrdiff_t(cut))); },
                "truncated parts at " + std::to_string(cut));
    Bytes bad = bytes;
    bad[1] = 17;
    Rejects([&] { MiiParts{bad}; }, "parts archive count");
    bad = bytes;
    const std::size_t section = (bytes[4] << 24) | (bytes[5] << 16) | (bytes[6] << 8) | bytes[7];
    Set32(bad, section + 4 + 4, 0x10); // the second file starts after the third
    Rejects([&] { MiiParts{bad}; }, "parts offsets out of order");
    std::mt19937 random(11);
    for (int n = 0; n < 200; ++n) {
        Bytes garbage(std::size_t(random() % 1024));
        for (auto& v : garbage) v = std::uint8_t(random());
        Rejects([&] { MiiParts{garbage}; }, "garbage parts");
    }
    Rejects([&] { MiiParts::FromArchive(MakeU8({1}, {2})); }, "archive without RFL_Res.dat");
}

// ---------------------------------------------------------------- records

void TestRecords()
{
    // The port's record transport (rfl_character_transport.c) reads the
    // Wii's bit fields: ReadMiiFace must agree with it on every value.
    std::mt19937 random(3);
    for (int n = 0; n < 300; ++n) {
        std::string record(kMiiRecordBytes, '\0');
        for (auto& c : record) c = char(random());
        RFLiCharData d;
        ChargedDecodeRFLCharRecordBE(record.data(), &d);
        const MiiFace f = ReadMiiFace(record);
        const int expected[] = {d.favoriteColor, d.faceType, d.faceColor, d.faceTex, d.hairType, d.hairColor,
                                d.hairFlip, d.eyebrowType, d.eyebrowColor, d.eyebrowScale, d.eyebrowRotate,
                                d.eyebrowX, d.eyebrowY, d.eyeType, d.eyeColor, d.eyeScale, d.eyeRotate, d.eyeX,
                                d.eyeY, d.noseType, d.noseScale, d.noseY, d.mouthType, d.mouthColor, d.mouthScale,
                                d.mouthY, d.glassType, d.glassColor, d.glassScale, d.glassY, d.mustacheType,
                                d.beardType, d.beardColor, d.beardScale, d.beardY, d.moleType, d.moleScale, d.moleX,
                                d.moleY};
        for (std::size_t k = 0; k < kMiiFaceFields.size(); ++k)
            Check(f.*kMiiFaceFields[k].value == expected[k], std::string("record field ") + kMiiFaceFields[k].name);

        // Writing a face changes those bits only.
        MiiFace changed;
        for (const auto& field : kMiiFaceFields)
            changed.*field.value = field.min + int(random() % unsigned(field.max - field.min + 1));
        Check(ValidMiiFace(changed), "random face in range");
        std::string copy = record;
        WriteMiiFace(changed, copy);
        const MiiFace back = ReadMiiFace(copy);
        for (const auto& field : kMiiFaceFields) Check(back.*field.value == changed.*field.value, "face round trip");
        WriteMiiFace(f, copy);
        Check(copy == record, "writing the old face back restores every byte");
    }
    // Random Miis are what the Mii library accepts.
    for (std::uint32_t seed = 0; seed < 200; ++seed)
        Check(ValidMiiFace(ReadMiiFace(MakeRandomMii("Test", seed, 1700000000))), "random Mii face valid");
    MiiFace f;
    Check(ValidMiiFace(f), "default face valid");
    f.eyebrow_y = 2;
    Check(!ValidMiiFace(f), "eyebrow below its lowest place");
    f = {};
    f.eye_type = 48;
    Check(!ValidMiiFace(f), "expression-only eye type");
    Rejects([] { ReadMiiFace(std::string(73, '\0')); }, "short record");
    Check(FavoriteColor(5) == std::array<std::uint8_t, 3>{32, 72, 152}, "favourite colour blue");
}

// ---------------------------------------------------------------- drawing

Bytes ShapeFile(const std::vector<std::array<int, 3>>& positions, const std::vector<std::array<int, 3>>& normals,
                const std::vector<std::array<int, 2>>& texcoords, const std::vector<std::array<int, 3>>& triangles,
                bool with_texcoords, bool faceline)
{
    Bytes b = {'t', 'e', 's', 't'};
    if (faceline) b.resize(b.size() + 36, 0); // nose, beard and hair placements: 0
    Put16(b, unsigned(positions.size()));
    for (const auto& p : positions)
        for (int v : p) Put16(b, unsigned(v) & 0xFFFF);
    Put16(b, unsigned(normals.size()));
    for (const auto& n : normals)
        for (int v : n) Put16(b, unsigned(v) & 0xFFFF);
    if (with_texcoords) {
        Put16(b, unsigned(texcoords.size()));
        for (const auto& t : texcoords)
            for (int v : t) Put16(b, unsigned(v) & 0xFFFF);
    }
    b.push_back(1);
    b.push_back(std::uint8_t(triangles.size()));
    b.push_back(0x90); // GX_TRIANGLES
    for (const auto& v : triangles) {
        b.push_back(std::uint8_t(v[0])), b.push_back(std::uint8_t(v[1]));
        if (with_texcoords) b.push_back(std::uint8_t(v[2]));
    }
    return b;
}

Bytes EmptyShape(bool faceline = false)
{
    Bytes b = {'n', 'o', 'n', 'e'};
    if (faceline) b.resize(b.size() + 36, 0);
    Put16(b, 0);
    return b;
}

// An RFLiTexture: 32-byte header, then the image.
Bytes TextureFile(int format, int width, int height, const Bytes& image, int wrap_s = 0)
{
    Bytes b(32, 0);
    b[0] = std::uint8_t(format);
    b[2] = std::uint8_t(width >> 8), b[3] = std::uint8_t(width);
    b[4] = std::uint8_t(height >> 8), b[5] = std::uint8_t(height);
    b[6] = std::uint8_t(wrap_s);
    b[0x14] = b[0x15] = 1; // GX_LINEAR
    Set32(b, 0x1C, 32);
    b.insert(b.end(), image.begin(), image.end());
    return b;
}

// RGB5A3 8 x 8, every texel `texel`.
Bytes FilledRgb5a3(unsigned texel)
{
    Bytes image;
    for (int n = 0; n < 64; ++n) Put16(image, texel);
    return TextureFile(5, 8, 8, image);
}

// Parts with one face: a square facing the camera, 40 units across around
// its eye line, lit head on, wound clockwise on the screen (the GX's front
// faces); every other part empty or clear.
Archives SyntheticArchives()
{
    const Bytes clear = TextureFile(0, 8, 8, Bytes(32, 0)); // I4, transparent
    Archives a;
    for (auto& archive : a) archive = {clear};
    const int lo = 14 * 256 + 128, hi = 54 * 256 + 128, x = 20 * 256;
    const auto square = [&](bool texcoords, bool faceline) {
        return ShapeFile({{-x, lo, 0}, {x, lo, 0}, {x, hi, 0}, {-x, hi, 0}}, {{0, 0, 16384}},
                         {{0, 0}, {8192, 0}, {8192, 8192}, {0, 8192}},
                         {{0, 0, 0}, {2, 0, 2}, {1, 0, 1}, {0, 0, 0}, {3, 0, 3}, {2, 0, 2}}, texcoords, faceline);
    };
    a[3] = {square(true, true)};                                // faceline 0
    a[4] = {FilledRgb5a3(0x8000 | (31 << 10) | 31), // face texture 0: green 0, no paint
            FilledRgb5a3(0x8000 | (31 << 10) | (31 << 5))}; // 1: red, green full
    for (const int shape : {0, 5, 6, 8, 9, 13, 14, 16}) a[std::size_t(shape)] = {EmptyShape()};
    return a;
}

std::array<int, 4> Pixel(const RgbaImage& image, int x, int y)
{
    const std::uint8_t* p = image.pixels.data() + (std::size_t(y) * image.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

void TestDrawing()
{
    Archives archives = SyntheticArchives();
    const MiiParts parts(MakeParts(archives));
    MiiFace face;
    MiiHeadOptions options;

    // Skin (240, 216, 196) fully lit: 6 bits in the colour buffer, then the
    // RGB5A3 icon's 5 bits.
    const RgbaImage head = RenderMiiHead(parts, face, options);
    Check(head.width == 128 && head.height == 128 && head.pixels.size() == 128u * 128 * 4, "head size");
    Check(Pixel(head, 64, 64) == std::array<int, 4>{247, 222, 198, 255}, "lit skin at the Wii's precision");
    Check(Pixel(head, 0, 0)[3] == 0 && Pixel(head, 127, 127)[3] == 0, "transparent around the head");
    options.wii_precision = false;
    Check(Pixel(RenderMiiHead(parts, face, options), 64, 64) == std::array<int, 4>{240, 216, 196, 255},
          "lit skin at 8 bits");
    options = {};
    Check(RenderMiiHead(parts, face, options).pixels == head.pixels, "deterministic");

    // The square spans 40 of the 103.68 units the camera sees at its
    // distance: about 49 of 128 pixels, centred.
    int left = 128, right = -1;
    for (int x = 0; x < 128; ++x)
        if (Pixel(head, x, 64)[3]) left = std::min(left, x), right = std::max(right, x);
    Check(left >= 38 && left <= 41 && right >= 86 && right <= 89 && left + right == 127, "square placement");

    // The face texture by its green, its alpha standing in for the green.
    face.face_texture = 1;
    Check(Pixel(RenderMiiHead(parts, face, options), 64, 64) == std::array<int, 4>{255, 255, 0, 255},
          "face texture swap");
    face.face_texture = 0;

    // Backgrounds are opaque and exact.
    options.background = MiiHeadOptions::Background::Color;
    options.background_color = {10, 20, 30};
    Check(Pixel(RenderMiiHead(parts, face, options), 0, 0) == std::array<int, 4>{10, 20, 30, 255}, "colour background");
    options.background = MiiHeadOptions::Background::Favorite;
    face.favorite_color = 4;
    Check(Pixel(RenderMiiHead(parts, face, options), 2, 3) == std::array<int, 4>{0, 116, 40, 255},
          "favourite background");
    options = {};
    face = {};

    // Sizes and shapes.
    for (const auto& [w, h] : {std::pair{64, 64}, std::pair{256, 256}, std::pair{48, 96}, std::pair{200, 100}}) {
        options.width = w, options.height = h;
        const RgbaImage image = RenderMiiHead(parts, face, options);
        Check(image.width == w && image.height == h && Pixel(image, w / 2, h / 2)[3] == 255, "head of another size");
    }
    options = {};
    for (const auto& [w, h] : {std::pair{0, 128}, std::pair{128, 1025}}) {
        options.width = w, options.height = h;
        Rejects([&] { RenderMiiHead(parts, face, options); }, "head size out of range");
    }
    options = {};
    face.favorite_color = 15;
    Check(RenderMiiHead(parts, face, options).pixels == head.pixels, "unchecked favourite colour drawn");
    face.eye_type = 48;
    Rejects([&] { RenderMiiHead(parts, face, options); }, "face out of range");
    face = {};
    face.face_type = 1;
    Rejects([&] { RenderMiiHead(parts, face, options); }, "face whose shape the parts lack");
    face = {};

    // Damaged part files.
    const auto damaged = [&](std::size_t archive, Bytes file, const std::string& what) {
        Archives bad = archives;
        bad[archive][0] = std::move(file);
        const MiiParts p(MakeParts(bad));
        Rejects([&] { RenderMiiHead(p, face, options); }, what);
    };
    damaged(3, ShapeFile({{0, 0, 0}}, {{0, 0, 16384}}, {{0, 0}}, {{0, 0, 0}, {1, 0, 0}, {0, 0, 0}}, true, true),
            "shape index past its positions");
    damaged(3, Bytes(archives[3][0].begin(), archives[3][0].end() - 5), "shape cut short");
    damaged(4, TextureFile(5, 64, 64, Bytes(8, 0)), "texture cut short");
    damaged(4, TextureFile(14, 8, 8, Bytes(32, 0)), "texture format the parts never use");
    damaged(1, TextureFile(5, 0, 8, Bytes()), "texture without width");
}

// ---------------------------------------------------------------- real parts

void TestRealParts(const std::filesystem::path& image)
{
    const MiiParts parts = ReadMiiPartsFromImage(image);
    MiiHeadOptions options;
    options.width = options.height = 64;
    // Every value of every field at its ends: all the files a face can use.
    for (const auto& field : kMiiFaceFields)
        for (const int value : {field.min, field.max, (field.min + field.max) / 2}) {
            MiiFace face;
            face.*field.value = value;
            const RgbaImage head = RenderMiiHead(parts, face, options);
            Check(Pixel(head, 32, 32)[3] == 255 && Pixel(head, 0, 0)[3] == 0,
                  std::string("real head with ") + field.name + " " + std::to_string(value));
        }
    for (int type = 0; type <= 71; ++type) {
        MiiFace face;
        face.hair_type = type;
        face.hair_flip = type & 1;
        RenderMiiHead(parts, face, options);
        ++checks;
    }
    for (int expression = 0; expression < 7; ++expression) {
        options.expression = Expression(expression);
        RenderMiiHead(parts, ReadMiiFace(MakeRandomMii("Test", 5, 1700000000)), options);
        ++checks;
    }
    options = {};
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        const std::string record = MakeRandomMii("Test", seed, 1700000000);
        const RgbaImage head = RenderMiiHead(parts, record, options);
        Check(RenderMiiHead(parts, record, options).pixels == head.pixels, "real head deterministic");
        std::size_t covered = 0;
        for (std::size_t n = 3; n < head.pixels.size(); n += 4) covered += head.pixels[n] != 0;
        const double share = double(covered) / (128.0 * 128.0);
        Check(share > 0.2 && share < 0.65, "a real head covers a fair part of its icon");
    }
}
// The game's own icons (MiiManager::CreateIcon, 128 x 128 RGB5A3 in GX
// tiles) for the records of `folder`/records.bin, as `folder`/icons/<n>.rgb5a3
// holds them (made from a game image, so never in the repository): the same
// coverage, and colours at most two RGB5A3 steps off in a few pixels, where
// floating-point rounding lands on the other side of a step (or of a texel
// at the edge of an eye).
void TestGameIcons(const MiiParts& parts, const std::filesystem::path& folder)
{
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read " + path.string());
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string records = read(folder / "records.bin");
    int icons = 0;
    std::size_t pixels = 0, off = 0;
    for (std::size_t n = 0; n + kMiiRecordBytes <= records.size(); n += kMiiRecordBytes) {
        const auto path = folder / "icons" / (std::to_string(n / kMiiRecordBytes) + ".rgb5a3");
        if (!std::filesystem::exists(path)) continue;
        const std::string raw = read(path);
        Check(raw.size() >= 128 * 128 * 2, "game icon size");
        const RgbaImage head = RenderMiiHead(parts, std::string_view(records).substr(n, kMiiRecordBytes));
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const std::size_t at = (std::size_t(y / 4) * 32 + x / 4) * 32 + ((y % 4) * 4 + x % 4) * 2;
                const unsigned v = (std::uint8_t(raw[at]) << 8) | std::uint8_t(raw[at + 1]);
                const auto e5 = [](unsigned c) { return int((c << 3) | (c >> 2)); };
                const auto e4 = [](unsigned c) { return int((c << 4) | c); };
                int c[4];
                if (v & 0x8000) c[0] = e5((v >> 10) & 31), c[1] = e5((v >> 5) & 31), c[2] = e5(v & 31), c[3] = 255;
                else
                    c[0] = e4((v >> 8) & 15), c[1] = e4((v >> 4) & 15), c[2] = e4(v & 15),
                    c[3] = int(((v >> 12) << 5) | ((v >> 12) << 2) | ((v >> 12) >> 1));
                const auto ours = Pixel(head, x, y);
                Check((c[3] == 0) == (ours[3] == 0), "game icon coverage");
                if (!c[3]) continue;
                ++pixels;
                int worst = std::abs(c[3] - ours[3]);
                for (int k = 0; k < 3; ++k)
                    worst = std::max(worst, std::abs(std::min(255, (c[k] * 255 + c[3] / 2) / c[3]) - ours[std::size_t(k)]));
                Check(worst <= 17, "game icon colour within two steps");
                off += worst != 0;
            }
        ++icons;
    }
    Check(icons > 0, "game icons found");
    Check(off * 1000 <= pixels, "game icon colours differ in at most 0.1 % of the head pixels");
    std::cout << "game icons compared: " << icons << ", " << off << " of " << pixels << " head pixels off\n";
}
} // namespace

int main()
{
    try {
        TestU8();
        TestParts();
        TestRecords();
        TestDrawing();
        if (const char* disc = std::getenv("MSCHARGED_TEST_DISC"); disc && *disc) {
            TestRealParts(disc);
            std::cout << "real Mii parts checked: " << disc << "\n";
            if (const char* icons = std::getenv("MSCHARGED_TEST_MII_REFERENCE"); icons && *icons)
                TestGameIcons(ReadMiiPartsFromImage(disc), icons);
        } else {
            std::cout << "MSCHARGED_TEST_DISC not set: real Mii parts skipped\n";
        }
        std::cout << "mii_head: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mii_head: " << error.what() << " (after " << checks << " checks)\n";
        return 1;
    }
}
