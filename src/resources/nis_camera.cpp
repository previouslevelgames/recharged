#include "resources/nis_camera.h"
#include "resources/chunk_reader.h"

namespace mscharged::resources
{
NisCameraLayout ReadNisCameras(Bytes file)
{
    Require(!file.empty() && file.size() <= MaximumAssetBytes, "NIS chunk data is empty or exceeds 16 MiB");
    NisCameraLayout result;
    for (std::size_t offset = 0; offset < file.size();)
    {
        const auto chunk = ReadChunk(file, offset, file.size());
        Require(chunk.next <= file.size(), "NIS chunk padding exceeds the file");
        if (chunk.id == 0x8002500b)
        {
            // Original Nis has ten slots but indexes before checking its count.
            // Validate that boundary before constructing any native records.
            Require(result.cameras.size() < MaximumNisCameras, "NIS exceeds its ten camera slots");
            ReadCameraAnimation(file, offset, chunk.next);
            result.cameras.push_back({offset, chunk.next});
        }
        else if (chunk.id == 0x80017000) ++result.animation_chunks;
        else ++result.other_chunks; // Original Nis ignores unrecognized chunks.
        offset = chunk.next;
    }
    return result;
}
}
