// Synthetic file records built independently of the native structures.
#include "resources/static_model.h"
#include "resources/texture_bundle.h"
#include <bit>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

using namespace mscharged::resources;
namespace
{
using Buffer = std::vector<std::uint8_t>;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Put32(Buffer& data, std::size_t offset, std::uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) data.at(offset + i) = value >> (24 - i * 8); }
void Put16(Buffer& data, std::size_t offset, std::uint16_t value)
{ data.at(offset) = value >> 8; data.at(offset + 1) = value; }
void PutFloat(Buffer& data, std::size_t offset, float value) { Put32(data, offset, std::bit_cast<std::uint32_t>(value)); }
void Reject(const std::function<void()>& call, const char* reason)
{ bool failed = false; try { call(); } catch (const std::runtime_error&) { failed = true; } Check(failed, reason); }
struct ModelFixture
{
    Buffer data = Buffer(8);
    std::map<std::uint32_t, std::size_t> offsets;
    void Chunk(std::uint32_t id, const Buffer& payload, unsigned alignment = 0)
    {
        const auto header = data.size(); data.resize(header + 8);
        while (data.size() % (1u << alignment)) data.push_back(0);
        offsets[id] = data.size(); data.insert(data.end(), payload.begin(), payload.end());
        Put32(data, header, id | (alignment << 24)); Put32(data, header + 4, data.size() - header - 8);
        while (data.size() % 4) data.push_back(0);
    }
    explicit ModelFixture(bool fixed_uv = false, unsigned alignment = 5, unsigned copies = 1, unsigned index_count = 3)
    {
        Buffer parameters(fixed_uv ? 36 : 8); Put32(parameters, 0, 0x12345678); parameters[6] = 3;
        Chunk(0x1b016, parameters);
        Buffer indices(index_count * 2); Put16(indices, 2, 1); Put16(indices, 4, 2);
        Chunk(0x1b007, indices); // BE indices with a legitimate zero offset.
        Buffer vertices(36 + (fixed_uv ? 12 : 24));
        const float xyz[] = {-1, 0, 0, 1, 0, 0, 0, 1, 0};
        for (unsigned i = 0; i < 9; ++i) PutFloat(vertices, i * 4, xyz[i]);
        if (fixed_uv)
        {
            Put16(vertices, 36, 0xfc00); Put16(vertices, 38, 512);
            Put16(vertices, 40, 1024); Put16(vertices, 42, 0);
            Put16(vertices, 44, 512); Put16(vertices, 46, 1024);
        }
        else
        {
            const float uv[] = {0, 1, 1, 1, 0.5f, 0};
            for (unsigned i = 0; i < 6; ++i) PutFloat(vertices, 36 + i * 4, uv[i]);
        }
        Chunk(0x1b006, vertices, alignment); // Payload alignment pad is included in chunk size.
        Buffer streams(16); streams[5] = 12; streams[6] = 1;
        Put32(streams, 8, 36); streams[13] = fixed_uv ? 4 : 8; streams[14] = 4;
        Chunk(0x1b005, streams);
        Buffer packet(48); Put32(packet, 4, index_count); Put16(packet, 8, 3); packet[11] = 2;
        Put32(packet, 16, fixed_uv ? 0x2169db5c : 0x21db4385);
        Buffer packets;
        for (unsigned i = 0; i < copies; ++i) packets.insert(packets.end(), packet.begin(), packet.end());
        Chunk(0x1b004, packets);
        Buffer matrix(64);
        for (unsigned i = 0; i < 4; ++i) PutFloat(matrix, (i * 4 + i) * 4, 1);
        PutFloat(matrix, 48, 2); PutFloat(matrix, 52, 3); PutFloat(matrix, 56, 4);
        Chunk(0x1b002, matrix);
        Buffer model(12); Put32(model, 0, 0x87654321); Put32(model, 4, copies);
        Chunk(0x1b003, model); Put32(data, 0, 0x8001b000); Put32(data, 4, data.size() - 8);
    }
};
Buffer TextureFixture(std::uint32_t format = 3, std::uint16_t w = 4, std::uint16_t h = 4,
    std::size_t pixel_bytes = 64, unsigned levels = 1)
{
    const unsigned palette = format == 8 ? 2 : 0;
    Buffer data(32 + 16 + 32 + pixel_bytes + palette * 2, 0);
    Put32(data, 0, 0x50544c47); Put32(data, 4, 1);
    Put32(data, 16, 0x12345678); Put32(data, 20, 16); Put32(data, 24, 32 + pixel_bytes + palette * 2);
    Put32(data, 48, levels); Put32(data, 52, format); Put16(data, 62, w); Put16(data, 64, h); Put32(data, 68, palette);
    for (std::size_t i = 80; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
    return data;
}
void Models()
{
    const ModelFixture file;
    auto models = ReadStaticModels(file.data);
    Check(models.size() == 1 && models[0].id == 0x87654321 && models[0].packets.size() == 1, "Model metadata");
    const auto& packet = models[0].packets[0];
    Check(packet.texture == 0x12345678 && packet.texture_flags == 3 && packet.indices == std::vector<std::uint16_t>{0,1,2}, "Packet indices/binding");
    Check(packet.vertices[0].position == std::array<float,3>{1,3,4} && packet.vertices[2].position == std::array<float,3>{2,4,4}, "Big endian float/matrix transpose");
    Check(packet.vertices[2].uv == std::array<float,2>{0.5f,0}, "Float UV");
    auto rotated = file.data;
    const auto matrix = file.offsets.at(0x1b002);
    PutFloat(rotated, matrix, 0); PutFloat(rotated, matrix + 4, 1);
    PutFloat(rotated, matrix + 16, -1); PutFloat(rotated, matrix + 20, 0);
    const auto transformed = ReadStaticModels(rotated);
    Check(transformed[0].packets[0].vertices[0].position == std::array<float,3>{2,2,4}, "Original row-vector matrix rotation");
    const ModelFixture fixed(true); auto fixed_models = ReadStaticModels(fixed.data);
    Check(fixed_models[0].packets[0].vertices[0].uv == std::array<float,2>{-1,0.5f}, "Signed fixed 10-bit UV");
    const ModelFixture compact(false, 0);
    Buffer collection(8); collection.insert(collection.end(), compact.data.begin(), compact.data.end());
    Put32(collection, 0, 0x8001b100); Put32(collection, 4, collection.size() - 8);
    Check(ReadStaticModels(collection)[0].id == 0x87654321, "Static group collection");
    collection.insert(collection.end(), compact.data.begin(), compact.data.end());
    Put32(collection, 4, collection.size() - 8);
    Reject([&] { ReadStaticModels(collection); }, "Duplicate model ID in collection");
    const ModelFixture budget(false, 0, 65, 65535);
    Reject([&] { ReadStaticModels(budget.data); }, "Excessive decoded index budget");
    // Root/group malformed sizes are rejected before any host pointer can be fixed up.
    for (std::size_t size = 0; size < file.data.size(); ++size)
        Reject([&] { ReadStaticModels(Bytes(file.data).first(size)); }, "Truncated model accepted");
    auto bad = file.data;
    Put32(bad, file.offsets.at(0x1b004) + 0, 0xffffffff); Reject([&] { ReadStaticModels(bad); }, "Out of range index offset");
    bad = file.data; Put16(bad, file.offsets.at(0x1b007) + 4, 3); Reject([&] { ReadStaticModels(bad); }, "Out of range index");
    bad = file.data; PutFloat(bad, file.offsets.at(0x1b006), std::numeric_limits<float>::quiet_NaN()); Reject([&] { ReadStaticModels(bad); }, "NaN position");
    bad = file.data; Put32(bad, file.offsets.at(0x1b004) + 16, 0xdeadcafe); Reject([&] { ReadStaticModels(bad); }, "Unknown shader");
    bad = file.data; Put32(bad, file.offsets.at(0x1b004) + 40, 1); Reject([&] { ReadStaticModels(bad); }, "Skinned packet");
    bad = file.data; Put32(bad, file.offsets.at(0x1b004) + 32, 4); Reject([&] { ReadStaticModels(bad); }, "Short material record");
    bad = file.data; PutFloat(bad, file.offsets.at(0x1b002) + 12, 1); Reject([&] { ReadStaticModels(bad); }, "Projective matrix");
    bad = file.data; Put32(bad, file.offsets.at(0x1b003) + 4, 2); Reject([&] { ReadStaticModels(bad); }, "Invalid model packet count");
    bad = file.data; Put32(bad, 0, 0x8001b200); Reject([&] { ReadStaticModels(bad); }, "Vertex animation root");
    bad = file.data; Put32(bad, 8, 0x0601b016); Reject([&] { ReadStaticModels(bad); }, "Excessive alignment");
}
void Textures()
{
    const auto file = TextureFixture(); const auto textures = ReadTextureBundle(file);
    Check(textures.size() == 1 && textures[0].width == 4 && textures[0].gx_format == 6 && textures[0].pixels.size() == 64, "Texture metadata");
    Check(textures[0].pixels[0] == 80 && textures[0].pixels[63] == 143, "Texture tiles must retain Wii bytes");
    const unsigned sizes[] = {192,192,64,384,128,64,128,192,128};
    const unsigned gx[] = {4,5,14,6,1,0,1,3,9};
    for (unsigned format = 0; format < 9; ++format)
    {
        const auto test = TextureFixture(format, 9, 5, sizes[format]); const auto value = ReadTextureBundle(test)[0];
        Check(value.gx_format == gx[format] && value.pixels.size() == sizes[format], "Nonaligned tile physical size");
        if (format == 8) Check(value.palette_entries == 2 && value.palette.size() == 4 && value.palette[0] == std::uint8_t(208), "Palette byte order");
    }
    Check(ReadTextureBundle(TextureFixture(3,4,4,192,3))[0].pixels.size() == 192, "Small mip tile allocation");
    for (std::size_t size = 0; size < file.size(); ++size)
        Reject([&] { ReadTextureBundle(Bytes(file).first(size)); }, "Truncated texture accepted");
    auto bad = file; Put32(bad, 4, 0xffffffff); Reject([&] { ReadTextureBundle(bad); }, "Dictionary overflow");
    bad = file; Put32(bad, 20, 0xfffffff0); Reject([&] { ReadTextureBundle(bad); }, "Data-relative texture offset");
    bad = file; Put32(bad, 48, 0x5f6c6669); Reject([&] { ReadTextureBundle(bad); }, "Animated texture");
    bad = file; Put32(bad, 48, 4); Reject([&] { ReadTextureBundle(bad); }, "Excess mip count");
    bad = file; Put32(bad, 52, 9); Reject([&] { ReadTextureBundle(bad); }, "Unknown texture format");
    bad = file; Put16(bad, 62, 0); Reject([&] { ReadTextureBundle(bad); }, "Zero texture width");
    bad = file; bad[60] = 1; Reject([&] { ReadTextureBundle(bad); }, "Missing texture");
    bad = file; Put32(bad, 68, 2); Reject([&] { ReadTextureBundle(bad); }, "Palette on nonindexed texture");
    Buffer aliases(16 + 100 * 16 + 32 + 1024 * 1024);
    Put32(aliases, 0, 0x50544c47); Put32(aliases, 4, 100);
    for (unsigned i = 0; i < 100; ++i)
    {
        Put32(aliases, 16 + i * 16, i); Put32(aliases, 24 + i * 16, 32 + 1024 * 1024);
    }
    Put32(aliases, 1616, 1); Put32(aliases, 1620, 3); Put16(aliases, 1630, 512); Put16(aliases, 1632, 512);
    Reject([&] { ReadTextureBundle(aliases); }, "Aliased texture decoded allocation budget");
    bad = TextureFixture(8,4,4,32); Put32(bad, 68, 257); Reject([&] { ReadTextureBundle(bad); }, "Excessive CI8 palette");
}
}
int main()
{
    try { Models(); Textures(); std::cout << "Static Wii resource conversion and rejection checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
