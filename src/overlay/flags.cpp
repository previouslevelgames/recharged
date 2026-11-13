#include "overlay/flags.h"

#include <algorithm>

namespace mscharged::overlay
{
const std::vector<FlagCountry>& FlagCountries()
{
    static const std::vector<FlagCountry> countries{
#include "overlay/flag_countries.inc"
    };
    return countries;
}

int FlagCell(std::string_view code)
{
    const auto& countries = FlagCountries();
    if (code.size() != 2) return -1;
    const auto found = std::lower_bound(countries.begin(), countries.end(), code,
                                        [](const FlagCountry& country, std::string_view code) {
                                            return std::string_view(country.code, 2) < code;
                                        });
    return found != countries.end() && std::string_view(found->code, 2) == code ? int(found - countries.begin())
                                                                               : -1;
}

std::string_view FlagName(std::string_view code)
{
    const int cell = FlagCell(code);
    return cell < 0 ? std::string_view() : std::string_view(FlagCountries()[std::size_t(cell)].name);
}

FlagUv FlagCellUv(int cell)
{
    // The atlas: kFlagColumns cells wide, as many rows as the countries need.
    const int rows = (int(FlagCountries().size()) + kFlagColumns - 1) / kFlagColumns;
    const float width = float(kFlagColumns * kFlagWidth), height = float(rows * kFlagHeight);
    const int column = cell % kFlagColumns, row = cell / kFlagColumns;
    return {float(column * kFlagWidth) / width, float(row * kFlagHeight) / height,
            float((column + 1) * kFlagWidth) / width, float((row + 1) * kFlagHeight) / height};
}
} // namespace mscharged::overlay
