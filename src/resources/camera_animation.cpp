#include "resources/camera_animation.h"
#include "resources/chunk_reader.h"
#include <algorithm>
#include <map>

namespace mscharged::resources
{
CameraAnimation ReadCameraAnimation(Bytes file)
{
    Require(file.size() <= MaximumAssetBytes, "CAM exceeds the 16 MiB asset limit");
    const auto root = ReadChunk(file, 0, file.size());
    Require(root.id == 0x8002500b && root.next == file.size(), "Invalid CAM root or trailing data");
    std::map<std::uint32_t, Bytes> chunks;
    const auto first = std::size_t(root.payload.data() - file.data());
    const auto end = first + root.payload.size();
    for (auto offset = first; offset < end;)
    {
        auto chunk = ReadChunk(file, offset, end);
        Require(chunk.next <= end, "CAM child padding exceeds its container");
        Require(chunk.id >= 0x25000 && chunk.id <= 0x2500f && chunk.id != 0x2500b,
                "Unsupported CAM chunk");
        Require(chunks.emplace(chunk.id, chunk.payload).second, "Duplicate CAM chunk");
        offset = chunk.next;
    }
    auto get = [&](std::uint32_t id) {
        const auto found = chunks.find(id);
        Require(found != chunks.end(), "Missing required CAM chunk");
        return found->second;
    };
    const auto count_data = get(0x2500c);
    Require(count_data.size() == 4, "Invalid CAM key-count chunk");
    const auto count = U32(count_data, 0);
    // Original interpolation reads index + 1 below the end; a one-key asset
    // needs separate playback qualification. Reject it in this initial profile.
    Require(count >= 2 && count <= 65536, "CAM key count is outside the supported range (2..65536)");
    const auto name = get(0x25000);
    const auto nul = std::find(name.begin(), name.end(), 0);
    Require(nul != name.end() && nul != name.begin() && nul - name.begin() <= 255,
            "CAM name is empty, unterminated or exceeds 255 bytes");
    Require(std::all_of(name.begin(), nul, [](auto c) { return c >= 32 && c < 127; }),
            "Unsupported CAM name encoding");
    Require(name.size() == Align(std::size_t(nul - name.begin()) + 1, 4)
        && std::all_of(nul, name.end(), [](auto c) { return c == 0; }), "Invalid CAM name padding");

    // Ancillary exported channels are ignored by LoadAnimCameraData. Bound
    // their known record sizes; leave their unconsumed bytes uninterpreted.
    for (const auto& [id, data] : chunks)
    {
        std::size_t width = 0, records = 1;
        if (id >= 0x25003 && id <= 0x2500a)
        {
            records = count;
            width = (id == 0x25004 || id == 0x25007) ? 16 : (id >= 0x25009 ? 4 : 12);
        }
        else if (id == 0x25002 || id == 0x2500d) width = 64;
        else if (id == 0x25001 || id == 0x2500c || id == 0x2500e || id == 0x2500f) width = 4;
        if (width)
        {
            Require(data.size() == records * width, "CAM channel size disagrees with its key count");
        }
    }
    const auto positions = get(0x25003), targets = get(0x25006), rotations = get(0x25004);
    const auto fovs = get(0x25009), focals = get(0x2500a);
    CameraAnimation result;
    result.name.assign(name.begin(), nul);
    result.keys.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        auto& key = result.keys[i];
        for (unsigned j = 0; j < 3; ++j)
        {
            key.position[j] = F32(positions, i * 12 + j * 4);
            // Original 0x25006 is copied verbatim and remains published even
            // with quaternion orientation. It is not a bounded mesh position.
            key.target[j] = std::bit_cast<float>(U32(targets, i * 12 + j * 4));
            Require(std::isfinite(key.target[j]), "Nonfinite CAM target coordinate");
        }
        double norm = 0;
        for (unsigned j = 0; j < 4; ++j)
        {
            key.rotation[j] = F32(rotations, i * 16 + j * 4);
            norm += double(key.rotation[j]) * key.rotation[j];
        }
        Require(std::abs(norm - 1) <= 0.001, "CAM rotation is not a unit quaternion");
        key.fov = F32(fovs, i * 4); key.focal_length = F32(focals, i * 4);
        Require(key.fov > 0 && key.fov < 180, "CAM FOV is outside (0, 180)");
    }
    return result;
}
}
