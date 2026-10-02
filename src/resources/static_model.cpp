#include "resources/static_model.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace mscharged::resources
{
namespace
{
struct Chunk { std::uint32_t id; Bytes payload; std::size_t next; };
Chunk ReadChunk(Bytes file, std::size_t offset, std::size_t end)
{
    Require(end <= file.size() && offset <= end && end - offset >= 8, "Truncated RLG chunk header");
    const auto raw = U32(file, offset);
    const auto size = U32(file, offset + 4);
    const unsigned exponent = (raw >> 24) & 0x7f;
    Require(exponent <= 5, "Unsupported RLG alignment above the original 32-byte file alignment");
    Require(size <= end - offset - 8, "RLG chunk exceeds its container");
    const auto stop = offset + 8 + size;
    const auto start = Align(offset + 8, std::size_t(1) << exponent);
    Require(start <= stop, "RLG alignment exceeds its payload");
    return {raw & 0x80ffffff, Slice(file, start, stop - start), Align(stop, 4)};
}

std::size_t ParameterSize(std::uint32_t program)
{
    switch (program)
    {
    case 0x32475c7d: return 48; // MaskedSpecularFresnel
    case 0x2169db5c: return 36; // ScrollingDiffuse
    case 0x21db4385: case 0xd3e572da: return 8;
    default: throw std::runtime_error("Unsupported RLG material program in static preview");
    }
}
bool PrimitiveCount(std::uint8_t kind, std::size_t size)
{
    switch (kind)
    {
    case 0: return size >= 3 && size % 3 == 0;
    case 1: case 2: return size >= 3;
    case 3: return size >= 4 && size % 4 == 0;
    case 4: return size >= 2 && size % 2 == 0;
    case 5: return size >= 2;
    default: return false;
    }
}
struct Budget { std::size_t vertices = 0, indices = 0; };
void ReadGroup(Bytes file, std::size_t start, std::size_t end, std::vector<StaticModel>& result, Budget& budget)
{
    std::map<std::uint32_t, Bytes> chunks;
    unsigned count = 0;
    while (start < end)
    {
        const auto chunk = ReadChunk(file, start, end);
        Require(++count <= 32, "Too many RLG chunks");
        Require(chunk.id == 0x1b016 || chunk.id == 0x1b007 || chunk.id == 0x1b006
            || chunk.id == 0x1b005 || chunk.id == 0x1b004 || chunk.id == 0x1b002 || chunk.id == 0x1b003,
            "Unsupported RLG chunk: static preview does not support skinning or vertex animation");
        Require(chunks.emplace(chunk.id, chunk.payload).second, "Duplicate RLG chunk");
        Require(chunk.next <= end, "RLG padding exceeds its container");
        start = chunk.next;
    }
    Require(chunks.size() == 7, "Static RLG group is missing a required chunk");
    const auto params = chunks.at(0x1b016), indices = chunks.at(0x1b007), vertices = chunks.at(0x1b006);
    const auto streams = chunks.at(0x1b005), packets = chunks.at(0x1b004), matrices = chunks.at(0x1b002), models = chunks.at(0x1b003);
    Records(indices, 2, MaximumAssetBytes / 2);
    Records(streams, 8, 32768);
    const auto packet_count = Records(packets, 48, 4096), matrix_count = Records(matrices, 64, 4096);
    const auto model_count = Records(models, 12, 4096);
    Require(model_count && model_count + result.size() <= 4096, "Empty or excessive RLG model collection");
    std::size_t next_packet = 0;
    for (std::size_t m = 0; m < model_count; ++m)
    {
        StaticModel model;
        model.id = U32(models, m * 12);
        Require(std::none_of(result.begin(), result.end(), [&](const auto& value) { return value.id == model.id; }), "Duplicate RLG model ID");
        const auto n = U32(models, m * 12 + 4);
        Require(n && next_packet <= packet_count && n <= packet_count - next_packet, "Invalid RLG model packet range");
        for (std::size_t p = next_packet; p < next_packet + n; ++p)
        {
            const auto record = Slice(packets, p * 48, 48);
            Packet packet;
            packet.primitive = record[10];
            auto& material = packet.material;
            material.program = U32(record, 16);
            packet.raster = U32(record, 28);
            const auto parameters = Slice(params, U32(record, 32), ParameterSize(material.program));
            const bool masked = material.program == 0x32475c7d, scrolling = material.program == 0x2169db5c;
            for (unsigned i = 0; i < (masked ? 3u : 1u); ++i)
            {
                material.textures[i] = {U32(parameters, i * 8), parameters[i * 8 + 6]};
                Require(!(material.textures[i].flags & ~3u) && !parameters[i * 8 + 7], "Unsupported RLG texture binding flags");
            }
            if (masked || scrolling)
            {
                const unsigned first = masked ? 24 : 8, count = masked ? 4 : 2;
                for (unsigned i = 0; i < count; ++i)
                {
                    material.scalars[i] = F32(parameters, first + i * 4);
                    Require(std::abs(material.scalars[i]) <= 1e4f, "Excessive material scalar");
                }
                Require(!masked || (material.scalars[0] >= 0 && material.scalars[0] <= 1), "Invalid material specular amount");
                for (unsigned i = 0; i < (masked ? 2u : 5u); ++i)
                {
                    material.switches[i] = U32(parameters, (masked ? 40 : 16) + i * 4);
                    Require(material.switches[i] <= 1, "Invalid material boolean");
                }
            }
            Require(!U32(record, 40) && !U32(record, 44), "Skinned RLG packets are not supported by the static preview");
            const auto num_indices = U32(record, 4);
            const auto unique = U16(record, 8);
            Require(num_indices <= 65535 && PrimitiveCount(packet.primitive, num_indices) && unique, "Invalid RLG primitive or vertex count");
            Require((budget.vertices += unique) <= 1024 * 1024 && (budget.indices += num_indices) <= 4 * 1024 * 1024,
                "RLG decoded geometry budget exceeded");
            const auto index_offset = U32(record, 0), stream_offset = U32(record, 12);
            Require(index_offset % 2 == 0 && stream_offset % 8 == 0, "Misaligned RLG index/stream offset");
            const auto index_data = Slice(indices, index_offset, std::size_t(num_indices) * 2);
            for (std::size_t i = 0; i < num_indices; ++i)
            {
                const auto index = U16(index_data, i * 2);
                Require(index < unique, "RLG index exceeds its packet vertex array");
                packet.indices.push_back(index);
            }
            const auto stream_data = Slice(streams, stream_offset, std::size_t(record[11]) * 8);
            Require(record[11] >= 2 && record[11] <= 16, "Invalid RLG stream count");
            // Material programs bind streams by ordinal; reject mismatched layouts.
            const std::vector<unsigned> layout = masked ? std::vector<unsigned>{1,2,4,4,4,3}
                : scrolling ? std::vector<unsigned>{1,2,4,3}
                : material.program == 0xd3e572da ? std::vector<unsigned>{1,4,3} : std::vector<unsigned>{1,4};
            Require(record[11] == layout.size(), "RLG stream count does not match the material");
            std::vector<Bytes> stream_bytes;
            std::vector<unsigned> strides;
            for (std::size_t i = 0; i < layout.size(); ++i)
            {
                const auto stream = Slice(stream_data, i * 8, 8);
                const auto stride = stream[5], id = stream[6];
                const unsigned expected = id == 1 ? 12 : id == 2 ? 3
                    : id == 4 && material.program == 0x21db4385 ? 8 : 4;
                Require(id == layout[i] && stride == expected && !stream[7], "RLG stream layout does not match the material");
                stream_bytes.push_back(Slice(vertices, U32(stream, 0), std::size_t(unique) * stride));
                strides.push_back(stride);
            }
            const auto matrix_index = U32(record, 24);
            Require(matrix_index < matrix_count, "Invalid RLG matrix index");
            std::array<float, 16> matrix;
            for (std::size_t i = 0; i < 16; ++i) matrix[i] = F32(matrices, std::size_t(matrix_index) * 64 + i * 4);
            Require(matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0 && matrix[15] == 1, "Nonaffine RLG matrix is unsupported");
            for (std::size_t i = 0; i < unique; ++i)
            {
                const float x = F32(stream_bytes[0], i * 12), y = F32(stream_bytes[0], i * 12 + 4), z = F32(stream_bytes[0], i * 12 + 8);
                Vertex v;
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    v.position[axis] = x * matrix[axis] + y * matrix[4 + axis] + z * matrix[8 + axis] + matrix[12 + axis];
                    Require(std::isfinite(v.position[axis]) && std::abs(v.position[axis]) <= 1e7f, "Invalid transformed RLG position");
                }
                unsigned coordinate = 0;
                for (unsigned stream = 1; stream < layout.size(); ++stream)
                {
                    const auto bytes = stream_bytes[stream]; const auto stride = strides[stream];
                    if (layout[stream] == 3) std::copy_n(bytes.begin() + i * 4, 4, v.colour.begin());
                    else if (layout[stream] == 4)
                    {
                        auto& uv = coordinate == 0 ? v.uv : coordinate == 1 ? v.uv1 : v.uv2;
                        for (unsigned axis = 0; axis < 2; ++axis)
                            uv[axis] = stride == 8 ? F32(bytes, i * 8 + axis * 4)
                                : std::bit_cast<std::int16_t>(U16(bytes, i * 4 + axis * 2)) / 1024.0f;
                        ++coordinate;
                    }
                    else if (layout[stream] == 2)
                    {
                        // Inverse transpose of the asset's affine 3x3. Positions
                        // are baked above, so normals must follow that transform.
                        double cofactor[3][3];
                        for (unsigned row = 0; row < 3; ++row)
                            for (unsigned col = 0; col < 3; ++col)
                                cofactor[row][col] = double(matrix[((col+1)%3)*4+(row+1)%3]) * matrix[((col+2)%3)*4+(row+2)%3]
                                    - double(matrix[((col+2)%3)*4+(row+1)%3]) * matrix[((col+1)%3)*4+(row+2)%3];
                        const double det = matrix[0]*cofactor[0][0] + matrix[4]*cofactor[0][1] + matrix[8]*cofactor[0][2];
                        Require(std::isfinite(det) && std::abs(det) > 1e-12, "Singular RLG normal transform");
                        double length = 0;
                        for (unsigned row = 0; row < 3; ++row)
                        {
                            double value = 0;
                            for (unsigned col = 0; col < 3; ++col)
                                value += cofactor[row][col] * std::bit_cast<std::int8_t>(bytes[i * 3 + col]) / 64.0 / det;
                            v.normal[row] = static_cast<float>(value); length += value * value;
                        }
                        Require(std::isfinite(length) && length > 1e-12, "Invalid RLG normal");
                        for (auto& value : v.normal) value /= std::sqrt(length);
                    }
                }
                packet.vertices.push_back(v);
            }
            model.packets.push_back(std::move(packet));
        }
        next_packet += n;
        result.push_back(std::move(model));
    }
    Require(next_packet == packet_count, "Unreferenced RLG packets");
}
}

std::vector<StaticModel> ReadStaticModels(Bytes data)
{
    Require(data.size() <= MaximumAssetBytes, "RLG exceeds the static preview size limit");
    const auto root = ReadChunk(data, 0, data.size());
    Require(root.next == data.size(), "Unexpected trailing RLG data");
    const auto root_start = static_cast<std::size_t>(root.payload.data() - data.data());
    const auto root_end = root_start + root.payload.size();
    std::vector<StaticModel> result;
    Budget budget;
    if (root.id == 0x8001b000) ReadGroup(data, root_start, root_end, result, budget);
    else if (root.id == 0x8001b100)
    {
        for (auto offset = root_start; offset < root_end;)
        {
            const auto group = ReadChunk(data, offset, root_end);
            Require(group.id == 0x8001b000, "Unsupported RLG model collection member");
            const auto start = static_cast<std::size_t>(group.payload.data() - data.data());
            ReadGroup(data, start, start + group.payload.size(), result, budget);
            Require(group.next <= root_end, "RLG group padding exceeds collection");
            offset = group.next;
        }
    }
    else throw std::runtime_error("Unsupported RLG root: only static model groups are enabled");
    Require(!result.empty(), "Empty RLG model collection");
    return result;
}
}
