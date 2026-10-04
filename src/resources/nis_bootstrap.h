#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <string>
#include <vector>

namespace mscharged::resources
{
struct NisAnimationProxy
{
    std::string name;
    std::array<float, 2> position{};
    std::uint16_t direction = 0;
};
struct NisDictionaryEntry
{
    std::string name;
    std::uint32_t bytes = 0, balls = 0, cameras = 0;
    std::array<float, 3> center{}, minimum{}, maximum{};
    // Retail files contain up to eight starts. The reconstructed NisHeader's
    // four-element member cannot safely hold those records on a native host.
    std::vector<std::array<float, 3>> animation_starts;
    std::vector<NisAnimationProxy> proxies;
};
// Owned, bounded representations of the original text formats. These neither
// construct NisPlayer nor publish script/audio/character readiness.
std::vector<std::string> ReadNisNameList(Bytes bytes);
std::vector<NisDictionaryEntry> ReadNisDictionary(Bytes bytes);
}
