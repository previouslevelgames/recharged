// Super Team tweak: the robot model and textures made from the GameCube robot
// (src/platform/super_team.cpp), on synthetic files of both games' formats.
#include "platform/super_team.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace mscharged::platform;
using Bytes = std::vector<std::uint8_t>;

namespace
{
int failures = 0, checks = 0;

void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

void U8(Bytes& b, std::uint8_t v) { b.push_back(v); }
void U16(Bytes& b, std::uint16_t v)
{
    b.push_back(std::uint8_t(v >> 8));
    b.push_back(std::uint8_t(v));
}
void U32(Bytes& b, std::uint32_t v)
{
    U16(b, std::uint16_t(v >> 16));
    U16(b, std::uint16_t(v));
}
void F32(Bytes& b, float v)
{
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    U32(b, bits);
}
void Append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }
std::uint32_t Get32(const Bytes& b, std::size_t o)
{
    return std::uint32_t(b[o]) << 24 | std::uint32_t(b[o + 1]) << 16 | std::uint32_t(b[o + 2]) << 8 | b[o + 3];
}
float GetF(const Bytes& b, std::size_t o)
{
    std::uint32_t bits = Get32(b, o);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

// A Charged chunk (no alignment bits): the next one is 4-aligned, so the data is padded.
Bytes Chunk(std::uint32_t id, const Bytes& data)
{
    Bytes out;
    U32(out, id);
    U32(out, std::uint32_t((data.size() + 3) & ~std::size_t(3)));
    Append(out, data);
    while (out.size() % 4) out.push_back(0);
    return out;
}

// A GameCube chunk (and both games' skeletons): the next one right after the data.
Bytes GcChunk(std::uint32_t id, const Bytes& data)
{
    Bytes out;
    U32(out, id);
    U32(out, std::uint32_t(data.size()));
    Append(out, data);
    return out;
}

void Bind(Bytes& b, std::uint32_t bone, float x)
{
    U32(b, bone);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) F32(b, r == 3 ? (c == 0 ? x : c == 3 ? 1.0f : 0.0f) : (r == c ? 1.0f : 0.0f));
}

constexpr std::uint32_t kBone0 = 0x100;
std::uint32_t BoneId(int i) { return kBone0 + std::uint32_t(i); }

// A chain of bones along x, bone i at x = i * spacing.
Bytes Skeleton(int bones, float spacing)
{
    Bytes ids, parents, offsets, body;
    for (int i = 0; i < bones; ++i)
    {
        U32(ids, BoneId(i));
        U32(parents, std::uint32_t(i - 1));
        F32(offsets, i ? spacing : 0.0f);
        F32(offsets, 0);
        F32(offsets, 0);
    }
    Append(body, GcChunk(0x18003, ids));
    Append(body, GcChunk(0x18009, parents));
    Append(body, GcChunk(0x02018010, offsets));
    return GcChunk(0x80018000, body);
}

// The GameCube robot: bones at x = 0, 1, 2 ...; per bone one triangle at
// local x 0.5, z 0 to 1 (the robot is one unit high).
Bytes Robot(int bones)
{
    Bytes verts, positions, normals, uvs, glows, indices, stitch, map, binds;
    const int count = bones * 3;
    for (int i = 0; i < bones; ++i)
    {
        const float corners[3][3] = {{i + 0.5f, 0, 0}, {i + 0.5f, 0.25f, 1}, {i + 0.5f, -0.25f, 1}};
        for (const auto& c : corners)
        {
            for (float v : c) F32(positions, v);
            U8(normals, 0);
            U8(normals, 0);
            U8(normals, 64);
            U16(uvs, std::uint16_t(std::int16_t(c[1] * 1024 + 512)));
            U16(uvs, std::uint16_t(std::int16_t(c[2] * 1024)));
            U16(glows, 0);
            U16(glows, 0);
        }
        Bind(binds, BoneId(i), float(i));
        U32(map, BoneId(i));
        U32(map, std::uint32_t(i * 3)); // the bone's matrix register
    }
    for (int i = 0; i < count; ++i)
    {
        U16(indices, std::uint16_t(i));
        U8(stitch, std::uint8_t((i / 3) * 3));
    }
    std::uint32_t pos = 0, nrm = std::uint32_t(positions.size()), uv = nrm + std::uint32_t(normals.size()),
                  glow = uv + std::uint32_t(uvs.size());
    Append(verts, positions);
    Append(verts, normals);
    Append(verts, uvs);
    Append(verts, glows);
    Bytes streams;
    const std::uint32_t layout[4][3] = {{pos, 0, 12}, {nrm, 1, 3}, {uv, 3, 4}, {glow, 6, 4}};
    for (const auto& s : layout)
    {
        U32(streams, s[0]);
        U8(streams, std::uint8_t(s[1]));
        U8(streams, std::uint8_t(s[2]));
    }
    Bytes packet;
    U32(packet, 0);
    U32(packet, 0); // indices at 0
    U16(packet, std::uint16_t(count));
    U8(packet, 0); // triangles
    U8(packet, 4);
    U32(packet, 0); // streams at 0
    packet.resize(0x4A, 0);
    Bytes models;
    U32(models, 1);
    U32(models, 0x67C6069A);
    U32(models, 0);
    U32(models, 0);
    Bytes stitchChunk;
    U32(stitchChunk, 1);
    U32(stitchChunk, 0);
    Append(stitchChunk, stitch);
    Bytes skin;
    Append(skin, GcChunk(0x1B00A, binds));
    Append(skin, GcChunk(0x1B00B, {}));
    Append(skin, GcChunk(0x1B00B, map));
    Append(skin, GcChunk(0x1B010, stitchChunk));
    Bytes body;
    Append(body, GcChunk(0x1B003, models));
    Append(body, GcChunk(0x1B004, packet));
    Append(body, GcChunk(0x1B005, streams));
    Append(body, GcChunk(0x1B006, verts));
    Append(body, GcChunk(0x1B007, indices));
    Append(body, GcChunk(0x8001B008, skin));
    return GcChunk(0x8001B000, body);
}

constexpr std::uint32_t kCharacterId = 0x12345678;
constexpr std::uint32_t kDamage1 = 0xD1D1D1D1, kDamage2 = 0xD2D2D2D2, kReflection = 0xEEEEEEEE;

// A Charged character: one packet in the fielder material, three vertices of
// height 3 bound to every bone, binds at x = 0, 2, 4 ...
Bytes Character(int bones)
{
    Bytes params(104, 0);
    auto put = [&](std::size_t o, std::uint32_t v) {
        params[o] = std::uint8_t(v >> 24);
        params[o + 1] = std::uint8_t(v >> 16);
        params[o + 2] = std::uint8_t(v >> 8);
        params[o + 3] = std::uint8_t(v);
    };
    put(0x00, 0xAAAAAAAA);
    put(0x08, kReflection);
    put(0x10, 0xBBBBBBBB);
    put(0x18, 0xCCCCCCCC);
    put(0x20, kDamage1);
    put(0x28, kDamage2);
    put(0x48, 1); // fresnel ramp
    Bytes verts, indices, map, binds;
    for (int k = 0; k < 3; ++k)
    {
        F32(verts, 0);
        F32(verts, 0);
        F32(verts, float(k)); // z 0, 1, 2 ... the third vertex below sets 3
    }
    // fix the last z to 3 for a height of 3
    std::memcpy(&verts[verts.size() - 4], "\x40\x40\x00\x00", 4);
    std::uint32_t indexStream = std::uint32_t(verts.size());
    for (int k = 0; k < 3; ++k) U32(verts, std::uint32_t(std::min(k, bones - 1)) << 24);
    std::uint32_t weightStream = std::uint32_t(verts.size());
    for (int k = 0; k < 3; ++k)
    {
        F32(verts, 1);
        F32(verts, 0);
        F32(verts, 0);
        F32(verts, 0);
    }
    for (int k = 0; k < 3; ++k) U16(indices, std::uint16_t(k));
    for (int i = 0; i < bones; ++i)
    {
        U32(map, BoneId(i));
        Bind(binds, BoneId(i), 2.0f * float(i));
    }
    Bytes streams;
    const std::uint32_t layout[3][3] = {{0, 12, 1}, {indexStream, 4, 7}, {weightStream, 16, 5}};
    for (const auto& s : layout)
    {
        U32(streams, s[0]);
        U8(streams, 0);
        U8(streams, std::uint8_t(s[1]));
        U8(streams, std::uint8_t(s[2]));
        U8(streams, 0);
    }
    Bytes packet;
    U32(packet, 0);
    U32(packet, 3);
    U16(packet, 3);
    U8(packet, 0);
    U8(packet, 3);
    U32(packet, 0);
    U32(packet, 0x1ACE1D01);
    U32(packet, 0);
    U32(packet, 0);
    U32(packet, 0x000D0007);
    U32(packet, 0);
    U32(packet, 0);
    U32(packet, 0);
    U32(packet, 0);
    Bytes matrix;
    for (int k = 0; k < 16; ++k) F32(matrix, k % 5 == 0 ? 1.0f : 0.0f);
    Bytes models;
    U32(models, kCharacterId);
    U32(models, 1);
    U32(models, 0);
    Bytes morph;
    U32(morph, 1);
    U32(morph, 0x4D4D4D4D);
    U32(morph, 16);
    U32(morph, 1);
    U32(morph, 0);
    Bytes skin;
    Append(skin, Chunk(0x1B00B, map));
    Append(skin, Chunk(0x1B00A, binds));
    Append(skin, Chunk(0x1B00C, morph));
    Bytes body;
    Append(body, Chunk(0x1B016, params));
    Append(body, Chunk(0x1B007, indices));
    Append(body, Chunk(0x1B006, verts));
    Append(body, Chunk(0x1B005, streams));
    Append(body, Chunk(0x1B004, packet));
    Append(body, Chunk(0x1B002, matrix));
    Append(body, Chunk(0x1B003, models));
    Append(body, Chunk(0x8001B008, skin));
    return Chunk(0x8001B000, body);
}

// A GameCube texture bundle with an 8x8 CMPR body texture.
Bytes Textures()
{
    Bytes texture(32, 0);
    texture[3] = 1;
    texture[7] = 2;
    texture[15] = 8;
    texture[17] = 8;
    for (int b = 0; b < 4; ++b)
    {
        U16(texture, 0x8410); // grey
        U16(texture, 0x0000);
        U32(texture, 0);
    }
    Bytes out;
    U32(out, 0x50544C47);
    U32(out, 1);
    out.resize(32, 0);
    U32(out, kSuperTeamBody);
    U32(out, 0);
    U32(out, std::uint32_t(texture.size()));
    U32(out, 0);
    Append(out, texture);
    return out;
}

struct Made
{
    std::uint32_t modelId = 0, packets = 0;
    std::vector<std::uint32_t> order;
    bool streams = true, material = true, mapsFit = true;
    std::size_t maxBones = 0;
    std::vector<float> positions; // x y z per vertex
    std::uint32_t diffuse = 0, reflection = 0, damage1 = 0, damage2 = 0, morphs = 0;
};

// Reads the made model back (the layout the game's RLGReader expects).
bool ReadMade(const Bytes& b, Made& m)
{
    if (b.size() < 8 || Get32(b, 0) != 0x8001B000 || Get32(b, 4) + 8 > b.size()) return false;
    std::size_t o = 8, end = 8 + Get32(b, 4);
    std::size_t at[0x20] = {};
    std::size_t size[0x20] = {};
    std::size_t skin = 0, skinSize = 0;
    while (o + 8 <= end)
    {
        std::uint32_t id = Get32(b, o), n = Get32(b, o + 4);
        m.order.push_back(id);
        if (id == 0x8001B008)
        {
            skin = o + 8;
            skinSize = n;
        }
        else if ((id & 0xFFFFFF00) == 0x1B000)
        {
            at[id & 0x1F] = o + 8;
            size[id & 0x1F] = n;
        }
        o += 8 + n;
    }
    if (!at[3] || !at[4] || !at[5] || !at[6] || !at[7] || !at[0x16] || !skin) return false;
    m.modelId = Get32(b, at[3]);
    m.packets = Get32(b, at[3] + 4);
    if (size[4] != m.packets * 48) return false;
    std::vector<std::size_t> maps;
    for (std::size_t s = skin; s + 8 <= skin + skinSize;)
    {
        std::uint32_t id = Get32(b, s), n = Get32(b, s + 4);
        if (id == 0x1B00B) maps.push_back(n / 4);
        if (id == 0x1B00C) m.morphs = Get32(b, s + 8);
        s += 8 + n;
    }
    m.mapsFit = maps.size() == m.packets;
    for (std::size_t c : maps) m.maxBones = std::max(m.maxBones, c);
    for (std::uint32_t p = 0; p < m.packets; ++p)
    {
        std::size_t pk = at[4] + p * 48;
        std::uint32_t unique = std::uint32_t(b[pk + 8]) << 8 | b[pk + 9];
        std::uint32_t streamOffset = Get32(b, pk + 12), params = Get32(b, pk + 32);
        m.material = m.material && Get32(b, pk + 16) == 0x1ACE1D01 && b[pk + 11] == 10 && b[pk + 10] == 0;
        const std::uint8_t ids[10] = {1, 2, 4, 4, 4, 4, 4, 4, 7, 5};
        for (int s = 0; s < 10; ++s) m.streams = m.streams && b[at[5] + streamOffset + 8 * std::size_t(s) + 6] == ids[s];
        std::uint32_t pos = Get32(b, at[5] + streamOffset);
        for (std::uint32_t v = 0; v < unique; ++v)
            for (int k = 0; k < 3; ++k) m.positions.push_back(GetF(b, at[6] + pos + v * 12 + 4 * std::size_t(k)));
        m.diffuse = Get32(b, at[0x16] + params);
        m.reflection = Get32(b, at[0x16] + params + 0x08);
        m.damage1 = Get32(b, at[0x16] + params + 0x20);
        m.damage2 = Get32(b, at[0x16] + params + 0x28);
    }
    return true;
}

bool Near(const std::vector<float>& positions, float x, float y, float z)
{
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3)
        if (std::fabs(positions[i] - x) < 1e-4f && std::fabs(positions[i + 1] - y) < 1e-4f
            && std::fabs(positions[i + 2] - z) < 1e-4f)
            return true;
    return false;
}
} // namespace

int main()
{
    // Three bones: the robot (one unit high, bones one apart) onto a character
    // three units high with bones two apart.
    {
        Bytes model = Robot(3), skeleton = Skeleton(3, 1.0f), textures = Textures();
        SuperTeamSource source{model, skeleton, textures};
        Check(ValidSuperTeamSource(source), "The synthetic Super Team files are valid");
        Bytes character = Character(3), characterSkeleton = Skeleton(3, 2.0f);
        auto made = MakeSuperTeamModel(source, character, characterSkeleton);
        Made m;
        Check(!made.empty() && ReadMade(Bytes(made.begin(), made.end()), m), "A robot model is made");
        const std::vector<std::uint32_t> order = {0x1B016, 0x1B007, 0x1B006, 0x1B005, 0x1B004, 0x1B002, 0x1B003,
            0x8001B008};
        Check(m.order == order, "The model's chunks come in the game's order");
        Check(m.modelId == SuperTeamModelId(kCharacterId) && m.modelId != kCharacterId,
            "The robot model has its own id");
        Check(m.material && m.streams && m.mapsFit, "Fielder material, its ten streams and one bone map per packet");
        Check(m.diffuse == kSuperTeamBody && m.reflection == kReflection && m.damage1 == kDamage1
                && m.damage2 == kDamage2,
            "The robot's body texture with the character's reflection and damage layers");
        Check(m.morphs == 1, "The character's morph slots stay (without deltas)");
        // Bone 0 spans to its child: local x 0.5 stretches by 2 (joint 2 apart
        // instead of 1); across, the height ratio 3 applies.
        Check(Near(m.positions, 1.0f, 0.0f, 0.0f) && Near(m.positions, 1.0f, 0.75f, 3.0f),
            "A stretched bone's part lies between the character's joints");
        // The last bone has no child joint: the height ratio along it too.
        Check(Near(m.positions, 4.0f + 1.5f, 0.0f, 0.0f), "A bone without a child joint scales with the height");

        auto bundle = MakeSuperTeamTextures(source);
        std::span<const std::uint8_t> body = FindBundleTexture(bundle, kSuperTeamBody);
        gx::HostVector<gx::Level> levels;
        std::uint32_t format = 0;
        Check(!bundle.empty() && std::memcmp(bundle.data(), "PTLG", 4) == 0
                && Get32(Bytes(bundle.begin(), bundle.end()), 4) == 3,
            "Three textures in a Charged bundle");
        Check(!body.empty() && gx::Levels(body, format, levels) && format == gx::kCMPR && levels[0].width == 8,
            "The body texture keeps its size");

        // Every truncation of every input: no robot, no crash.
        bool safe = true;
        for (std::size_t n = 0; n < model.size(); n += 3)
        {
            Bytes cut(model.begin(), model.begin() + std::ptrdiff_t(n));
            SuperTeamSource broken{cut, skeleton, textures};
            safe = safe && !ValidSuperTeamSource(broken)
                && MakeSuperTeamModel(broken, character, characterSkeleton).empty();
        }
        for (std::size_t n = 0; n < character.size(); n += 3)
        {
            Bytes cut(character.begin(), character.begin() + std::ptrdiff_t(n));
            safe = safe && MakeSuperTeamModel(source, cut, characterSkeleton).empty();
        }
        for (std::size_t n = 0; n < characterSkeleton.size(); n += 3)
        {
            Bytes cut(characterSkeleton.begin(), characterSkeleton.begin() + std::ptrdiff_t(n));
            safe = safe && MakeSuperTeamModel(source, character, cut).empty();
        }
        for (std::size_t n = 0; n < textures.size(); ++n)
        {
            Bytes cut(textures.begin(), textures.begin() + std::ptrdiff_t(n));
            safe = safe && MakeSuperTeamTextures({model, skeleton, cut}).empty();
        }
        Check(safe, "Truncated inputs give no robot");
        // Bytes flipped one at a time: whatever comes out, nothing crashes.
        for (std::size_t n = 0; n < character.size(); ++n)
        {
            Bytes flipped = character;
            flipped[n] ^= 0xA5;
            (void)MakeSuperTeamModel(source, flipped, characterSkeleton);
        }
        for (std::size_t n = 0; n < model.size(); ++n)
        {
            Bytes flipped = model;
            flipped[n] ^= 0xA5;
            (void)MakeSuperTeamModel({flipped, skeleton, textures}, character, characterSkeleton);
        }
        Check(true, "Flipped bytes do not crash the converter");
    }
    // Twenty bones: the game draws at most nine per packet.
    {
        Bytes model = Robot(20), skeleton = Skeleton(20, 1.0f), textures = Textures();
        Bytes character = Character(20), characterSkeleton = Skeleton(20, 1.5f);
        auto made = MakeSuperTeamModel({model, skeleton, textures}, character, characterSkeleton);
        Made m;
        Check(!made.empty() && ReadMade(Bytes(made.begin(), made.end()), m), "A twenty-bone robot is made");
        Check(m.packets >= 3 && m.maxBones <= 9 && m.mapsFit, "Packets split at nine bones");
    }
    // A character without the fielder material, and garbage sources.
    {
        Bytes model = Robot(3), skeleton = Skeleton(3, 1.0f), textures = Textures();
        Bytes character = Character(3);
        for (std::size_t i = 0; i + 4 <= character.size(); ++i)
            if (Get32(character, i) == 0x1ACE1D01) character[i + 3] = 0x02;
        Check(MakeSuperTeamModel({model, skeleton, textures}, character, Skeleton(3, 2.0f)).empty(),
            "A character without the fielder material keeps its look");
        Bytes garbage(64, 0x5A);
        Check(!ValidSuperTeamSource({garbage, garbage, garbage}) && !ValidSuperTeamSource({}),
            "Other files are not the Super Team's");
    }
    Check(SuperTeamKeepsLook(1) && SuperTeamKeepsLook(11) && SuperTeamKeepsLook(16) && !SuperTeamKeepsLook(0)
            && !SuperTeamKeepsLook(6) && !SuperTeamKeepsLook(19),
        "Bowser, Petey and Boo keep their look");

    std::printf("super_team: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
