#pragma once

#include <string_view>
#include <vector>

// The flags beside players' names (a player's country, from the profile
// server). assets/flags/flags.png (its README) holds every ISO 3166-1
// country's flag, kFlagWidth x kFlagHeight pixels, kFlagColumns to a row, in
// the order of FlagCountries(); tools/flag_atlas.py makes both.
namespace mscharged::overlay
{
inline constexpr int kFlagWidth = 32, kFlagHeight = 24, kFlagColumns = 16;
// The picture key (OverlayImages) the host answers with the atlas's texture.
inline constexpr std::string_view kFlagAtlas = "asset:flags";

struct FlagCountry
{
    const char* code; // two capital letters
    const char* name; // in English, UTF-8
};

// Every country with a flag, by its letters: the atlas's order.
const std::vector<FlagCountry>& FlagCountries();
// The atlas cell of `code` (two capital letters), or -1 for none.
int FlagCell(std::string_view code);
// The country's English name, or empty.
std::string_view FlagName(std::string_view code);

// Where a cell is in the atlas, as texture coordinates.
struct FlagUv
{
    float u0, v0, u1, v1;
};
FlagUv FlagCellUv(int cell);
} // namespace mscharged::overlay
