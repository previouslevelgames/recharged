#include "resources/static_model.h"
#include <bit>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <string>

using namespace mscharged::resources;
namespace
{
using Buffer = std::vector<std::uint8_t>;
unsigned checks = 0;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void Reject(const std::function<void()>& call)
{
    bool rejected = false;
    try { call(); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "Malformed effects geometry accepted");
}
void Put32(Buffer& b, std::size_t at, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) b.at(at + i) = value >> (24 - 8 * i);
}
void Put16(Buffer& b, std::size_t at, std::uint16_t value)
{
    b.at(at) = value >> 8; b.at(at + 1) = value;
}
void Float(Buffer& b, std::size_t at, float value) { Put32(b, at, std::bit_cast<std::uint32_t>(value)); }
void Append(Buffer& b, std::uint32_t id, const Buffer& contents)
{
    const auto start = b.size(); b.resize(start + 8);
    Put32(b, start, id); Put32(b, start + 4, contents.size());
    b.insert(b.end(), contents.begin(), contents.end());
    while (b.size() % 4) b.push_back(0);
}
struct Fixture
{
    Buffer bytes;
    std::map<std::uint32_t, std::size_t> offsets;
    std::size_t animation = 0, metadata = 0;
    explicit Fixture(bool constant = false, unsigned packets = 1)
    {
        Buffer group;
        auto chunk = [&](std::uint32_t id, const Buffer& payload)
        {
            offsets[id] = 24 + group.size(); // Two enclosing headers, then child header.
            Append(group, id, payload);
        };
        Buffer parameters(constant ? 24 : 8);
        Put32(parameters, 0, 0xabcdef01); parameters[6] = 3;
        if (constant) for (unsigned i = 0; i < 4; ++i) Float(parameters, 8 + i * 4, (i + 1) / 4.f);
        chunk(0x1b016, parameters);
        Buffer indices(6); Put16(indices, 2, 2); Put16(indices, 4, 1); chunk(0x1b007, indices);
        Buffer vertices(constant ? 48 : 72);
        for (unsigned i = 0; i < 9; ++i) Float(vertices, i * 4, float(i) - 4);
        for (unsigned i = 0; i < 6; ++i)
            if (constant) Put16(vertices, 36 + i * 2, std::uint16_t(std::int16_t((int(i) - 2) * 4096)));
            else Float(vertices, 36 + i * 4, (int(i) - 2) / 2.f);
        if (!constant) for (unsigned i = 0; i < 12; ++i) vertices[60 + i] = i + 10;
        chunk(0x1b006, vertices);
        Buffer streams(constant ? 16 : 24); streams[5] = 12; streams[6] = 1;
        Put32(streams, 8, 36); streams[13] = constant ? 4 : 8; streams[14] = 4;
        if (!constant) { Put32(streams, 16, 60); streams[21] = 4; streams[22] = 3; }
        chunk(0x1b005, streams);
        Buffer packet(packets * 48);
        for (unsigned p = 0; p < packets; ++p)
        {
            Put32(packet, p * 48 + 4, 3); Put16(packet, p * 48 + 8, 3);
            packet[p * 48 + 11] = constant ? 2 : 3;
            Put32(packet, p * 48 + 16, constant ? 0xee9d919d : 0x19065bf6);
        }
        chunk(0x1b004, packet);
        Buffer matrix(64); for (unsigned i = 0; i < 4; ++i) Float(matrix, i * 20, 1);
        Float(matrix, 48, 100); chunk(0x1b002, matrix); // Effects must retain local positions.
        Buffer model(12); Put32(model, 0, 0x10203040); Put32(model, 4, packets); chunk(0x1b003, model);
        Buffer skin; Append(skin, 0x1b00b, {}); Append(skin, 0x1b00a, {});
        Buffer skin_header(12); Put32(skin_header, 4, 0x100); Put32(skin_header, 8, 1);
        Append(skin, 0x1b00c, skin_header);
        metadata = 16 + group.size(); chunk(0x8001b008, skin);
        Buffer anim(28 + 2 * packets * 3 * 12);
        Put32(anim, 0, 0x10203040); Put32(anim, 4, 2); Put32(anim, 8, packets * 3);
        Put32(anim, 12, 12); Put32(anim, 16, 1); Put32(anim, 20, 1); Put32(anim, 24, 1);
        for (unsigned i = 0; i < packets * 18; ++i) Float(anim, 28 + i * 4, float(i) - 11.5f);
        Buffer animation_container; Append(animation_container, 0x1b201, anim);
        animation = 32 + group.size(); chunk(0x8001b200, animation_container);
        Buffer root; Append(root, 0x8001b000, group); Append(bytes, 0x80000001, root);
    }
};
void Generated()
{
    for (bool constant : {false, true}) for (unsigned packet_count : {1u, 2u})
    {
        const Fixture f(constant, packet_count); const auto result = ReadEffectsGeometry(f.bytes);
        Check(result.models.size() == 1 && result.animations.size() == 1, "Effects collections");
        const auto& model = result.models[0]; const auto& packet = model.packets[0];
        Check(model.id == 0x10203040 && model.packets.size() == packet_count, "Effects packet order/count");
        Check(packet.vertices[0].position == std::array<float,3>{-4,-3,-2}, "Effects positions remain local");
        Check(packet.indices == std::vector<std::uint16_t>{0,2,1}, "Effects big endian indices");
        Check(packet.vertices[0].uv == (constant ? std::array<float,2>{-2,-1} : std::array<float,2>{-1,-.5}), "Effects float / signed 12-fraction UVs");
        Check(packet.material.textures[0].texture == 0xabcdef01 && packet.material.textures[0].flags == 3, "Effects authored texture binding");
        if (constant) Check(packet.material.specular_colour == std::array<float,4>{.25,.5,.75,1}, "Constant RGBA floats");
        else Check(packet.vertices[0].colour == std::array<std::uint8_t,4>{10,11,12,13}, "Float material vertex colour");
        const auto& anim = result.animations[0];
        Check(anim.model == model.id && anim.frames == 2 && anim.vertices == packet_count * 3 && anim.stride == 12
            && anim.unknown == 1 && anim.streams == std::vector<std::uint32_t>{1}, "Animation source header");
        for (unsigned i = 0; i < anim.positions.size(); ++i) for (unsigned axis = 0; axis < 3; ++axis)
            Check(anim.positions[i][axis] == float(i * 3 + axis) - 11.5f, "Frame/packet/vertex animation order");
        const auto& skin = result.skin_metadata.at(model.id);
        Check(skin == Buffer(f.bytes.begin() + f.metadata, f.bytes.begin() + f.metadata + 44), "Complete original skin metadata retained");
        Reject([&] { ReadStaticModels(Bytes(f.bytes).subspan(8)); });
        for (std::size_t length = 0; length < f.bytes.size(); ++length)
            Reject([&] { ReadEffectsGeometry(Bytes(f.bytes).first(length)); });
        for (const auto [offset, value] : std::initializer_list<std::pair<unsigned,std::uint32_t>>{
            {0, 0x8001b100}, {unsigned(f.offsets.at(0x1b004) + 16), 0},
            {unsigned(f.animation), 0xffffffff}, {unsigned(f.animation + 4), 0},
            {unsigned(f.animation + 4), 4097}, {unsigned(f.animation + 8), 0},
            {unsigned(f.animation + 8), 4}, {unsigned(f.animation + 12), 16},
            {unsigned(f.animation + 20), 2}, {unsigned(f.animation + 24), 2},
            {unsigned(f.metadata + 8), 0x1b099}})
        {
            auto bad = f.bytes; Put32(bad, offset, value); Reject([&] { ReadEffectsGeometry(bad); });
        }
        auto bad = f.bytes; Float(bad, f.animation + 28, std::numeric_limits<float>::quiet_NaN());
        Reject([&] { ReadEffectsGeometry(bad); });
        bad = f.bytes; Put16(bad, f.offsets.at(0x1b007) + 2, 3); Reject([&] { ReadEffectsGeometry(bad); });
        bad = f.bytes; bad[f.offsets.at(0x1b005) + 13] = constant ? 8 : 4; Reject([&] { ReadEffectsGeometry(bad); });
        if (constant) for (float value : {-1.f, 1.01f, std::numeric_limits<float>::infinity()})
        {
            bad = f.bytes; Float(bad, f.offsets.at(0x1b016) + 8, value); Reject([&] { ReadEffectsGeometry(bad); });
        }
        // Duplicate model/animation IDs and an unassociated animation fail.
        Buffer duplicate; Append(duplicate, 0x80000001, Buffer(f.bytes.begin() + 8, f.bytes.end()));
        duplicate.insert(duplicate.end(), f.bytes.begin() + 8, f.bytes.end()); Put32(duplicate, 4, duplicate.size() - 8);
        Reject([&] { ReadEffectsGeometry(duplicate); });
    }
}
struct Hash
{
    std::uint64_t value = 0xcbf29ce484222325;
    void Byte(std::uint8_t b) { value = (value ^ b) * 0x100000001b3; }
    void Word(std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) Byte(v >> (24 - i * 8)); }
    void Float(float v) { Word(std::bit_cast<std::uint32_t>(v)); }
};
void Owned(const char* path, const char* oracle_path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read owned effects geometry");
    const Buffer bytes{std::istreambuf_iterator<char>(input), {}};
    const auto result = ReadEffectsGeometry(bytes);
    std::ifstream oracle(oracle_path);
    if (!oracle) throw std::runtime_error("Cannot read independent effects oracle");
    unsigned count = 0;
    for (const auto& model : result.models)
    {
        std::uint32_t id, program, texture; std::size_t frames, vertices, metadata_size;
        std::uint64_t positions, metadata, base_positions, uvs, indices;
        Check(bool(oracle >> std::hex >> id >> program >> texture >> positions >> metadata >> base_positions >> uvs >> indices
            >> std::dec >> frames >> vertices >> metadata_size), "Owned oracle row absent");
        Check(model.id == id && model.packets.size() == 1 && model.packets[0].material.program == program
            && model.packets[0].material.textures[0].texture == texture, "Owned effects source IDs / material");
        const auto& anim = result.animations.at(count++);
        Check(anim.model == id && anim.frames == frames && anim.vertices == vertices && anim.stride == 12
            && anim.unknown == 1 && anim.streams == std::vector<std::uint32_t>{1}, "Owned animation dimensions/source order");
        Hash position_hash, metadata_hash, base_hash, uv_hash, index_hash;
        for (const auto& position : anim.positions) for (const auto v : position) position_hash.Float(v);
        if (const auto it = result.skin_metadata.find(id); it != result.skin_metadata.end())
        {
            Check(it->second.size() == metadata_size, "Owned metadata extent");
            for (const auto b : it->second) metadata_hash.Byte(b);
        }
        else Check(metadata_size == 0, "Owned missing metadata");
        for (const auto& packet : model.packets)
        {
            for (const auto& v : packet.vertices)
            {
                for (const auto axis : v.position) base_hash.Float(axis);
                for (const auto axis : v.uv) uv_hash.Float(axis);
            }
            for (const auto i : packet.indices) { index_hash.Byte(i >> 8); index_hash.Byte(i); }
        }
        Check(position_hash.value == positions && metadata_hash.value == metadata && base_hash.value == base_positions
            && uv_hash.value == uvs && index_hash.value == indices, "Owned values differ from independent raw-byte oracle");
    }
    std::string extra; Check(!(oracle >> extra), "Extra owned oracle rows");
    std::cout << "Owned effects models/animations: " << result.models.size() << '/' << result.animations.size() << '\n';
}
}
int main(int argc, char** argv)
{
    try
    {
        Generated();
        if (argc == 4 && std::string(argv[1]) == "--owned") Owned(argv[2], argv[3]);
        else if (argc != 1) throw std::runtime_error("Use: effects_geometry_tests [--owned GEOMETRY ORACLE]");
        std::cout << checks << " effects geometry checks passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
