#include "resources/world_scene.h"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace mscharged::resources
{
AvailableWorldScene ReadAvailableWorldScene(Bytes resident, Bytes temporary)
{
    AvailableWorldScene result;
    std::vector<std::uint32_t> selected;
    for (const auto& record : ReadWorldObjectIndex(resident))
    {
        if (record.type == 0x10) { ++result.parent_records; continue; }
        try
        {
            // Validate the complete supported path before including an object.
            // In particular, a missing model/texture or invalid index must not
            // be misreported as an unimplemented object type.
            ReadStaticWorldScene(resident, temporary, std::span(&record.id, 1));
            selected.push_back(record.id);
        }
        catch (const UnsupportedResource& error)
        { result.unavailable.push_back({record.id, record.type, error.what()}); }
    }
    Require(!selected.empty(), "World contains no objects supported by the current static implementation");
    result.scene = ReadStaticWorldScene(resident, temporary, selected);
    return result;
}
StaticWorldScene ReadStaticWorldScene(Bytes resident, Bytes temporary,
    std::span<const std::uint32_t> selected)
{
    Require(!selected.empty() && selected.size() <= MaximumPreviewObjects,
        "World preview needs 1..256 explicit object IDs");
    StaticWorldScene result;
    result.objects = ReadStaticWorldObjects(resident, selected);
    std::map<std::uint32_t, std::size_t> models;
    std::set<std::uint32_t> textures;
    Bytes texture_data;
    std::size_t decoded_bytes = 0;
    for (const auto& object : result.objects)
    {
        if (models.contains(object.model)) continue;
        try
        {
            auto selected_model = ReadStaticWorldModel(temporary, object.model, ModelCoordinates::Local);
            for (const auto& packet : selected_model.model.packets)
            {
                const auto program = packet.material.program;
                if (program == 0x386ecbdd) throw UnsupportedResource("World preview does not submit shadow-volume models");
                const unsigned bindings = program == 0x112ab470 ? 4
                    : (program == 0x32475c7d || program == 0x32bc21e8 || program == 0x09609a35
                        || program == 0xf2d57ac6 || program == 0x845cad59) ? 3
                    : program == 0x3eccd955 ? 2 : 1;
                for (unsigned i = 0; i < bindings; ++i) textures.insert(packet.material.textures[i].texture);
                decoded_bytes += packet.vertices.size() * sizeof(Vertex) + packet.indices.size() * sizeof(std::uint16_t);
                Require(decoded_bytes <= 4 * MaximumAssetBytes, "Decoded world geometry exceeds its preview budget");
            }
            texture_data = selected_model.textures;
            models.emplace(object.model, result.models.size());
            result.models.push_back(std::move(selected_model.model));
        }
        catch (const UnsupportedResource& error)
        {
            std::ostringstream message;
            message << "World object 0x" << std::hex << object.id << " / model 0x" << object.model << ": " << error.what();
            throw UnsupportedResource(message.str());
        }
        catch (const std::exception& error)
        {
            std::ostringstream message;
            message << "World object 0x" << std::hex << object.id << " / model 0x" << object.model << ": " << error.what();
            throw std::runtime_error(message.str());
        }
    }
    result.textures = ReadTextureBundle(texture_data, {textures.begin(), textures.end()});
    std::array<double, 3> low{1e7,1e7,1e7}, high{-1e7,-1e7,-1e7};
    for (const auto& object : result.objects)
    {
        const auto& transform = object.transform;
        for (const auto& packet : result.models[models.at(object.model)].packets)
            for (const auto& vertex : packet.vertices)
                for (unsigned axis = 0; axis < 3; ++axis)
                {
                    double value = transform[12 + axis];
                    for (unsigned k = 0; k < 3; ++k) value += double(vertex.position[k]) * transform[k * 4 + axis];
                    Require(std::isfinite(value) && std::abs(value) <= 1e7, "Transformed world geometry exceeds its preview range");
                    low[axis] = std::min(low[axis], value); high[axis] = std::max(high[axis], value);
                }
    }
    double diameter2 = 0;
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        result.bounds.center[axis] = float((low[axis] + high[axis]) * .5);
        diameter2 += (high[axis] - low[axis]) * (high[axis] - low[axis]);
    }
    result.bounds.radius = float(std::sqrt(diameter2) * .5);
    Require(std::isfinite(result.bounds.radius) && result.bounds.radius > 1e-6f, "Selected world geometry has degenerate bounds");
    return result;
}
}
