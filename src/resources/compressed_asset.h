#pragma once
#include "resources/binary_reader.h"
#include <vector>

namespace mscharged::resources
{
// Charged .zlib files: BE decompressed byte count followed by one zlib stream.
// This bounded asset reader does not replace original NL streaming/cancellation.
std::vector<std::uint8_t> InflateAsset(Bytes data);
}
