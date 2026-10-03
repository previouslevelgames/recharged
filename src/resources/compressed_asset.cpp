#include "resources/compressed_asset.h"
#include <zlib.h>

namespace mscharged::resources
{
std::vector<std::uint8_t> InflateAsset(Bytes data)
{
    Require(data.size() > 4 && data.size() <= MaximumAssetBytes, "Invalid compressed asset size");
    const auto size = U32(data, 0);
    Require(size && size <= MaximumAssetBytes, "Decompressed asset exceeds its budget");
    std::vector<std::uint8_t> result(size);
    uLongf output = size;
    uLong input = data.size() - 4;
    const auto status = uncompress2(result.data(), &output, data.data() + 4, &input);
    Require(status == Z_OK && output == size && input == data.size() - 4,
        "Invalid compressed asset: stream, checksum, length or trailing data");
    return result;
}
}
