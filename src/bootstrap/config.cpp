#include "config.h"
#include "platform/path.h"

#include <fstream>
#include <stdexcept>
#include <string>

namespace mscharged
{
namespace
{
std::string Trim(const std::string& value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}
}

std::filesystem::path LoadDiscPath(const std::filesystem::path& config)
{
    std::ifstream input(config);
    if (!input)
        throw std::runtime_error("Cannot read " + PathUtf8(config)
                                 + ". Copy mscharged.ini.example to mscharged.ini and set [game] disc.");

    std::string line, disc;
    bool in_game = false;
    bool have_disc = false;
    size_t line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        if (line_number == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3); // UTF-8 BOM, common in Windows editors.
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line == "[game]")
        {
            in_game = true;
            continue;
        }
        const auto separator = line.find('=');
        if (!in_game || separator == std::string::npos
            || Trim(line.substr(0, separator)) != "disc" || have_disc)
            throw std::runtime_error("Invalid configuration at " + PathUtf8(config)
                                     + ":" + std::to_string(line_number)
                                     + ". Expected one [game] disc setting.");
        disc = Trim(line.substr(separator + 1));
        if (disc.size() >= 2 && disc.front() == '"' && disc.back() == '"')
            disc = disc.substr(1, disc.size() - 2);
        have_disc = true;
    }
    if (input.bad())
        throw std::runtime_error("Failed to read configuration: " + PathUtf8(config));
    if (disc.empty())
        throw std::runtime_error("Set [game] disc to an ISO or RVZ path in " + PathUtf8(config));
    const auto path = PathFromUtf8(disc);
    return path.is_absolute() ? path : config.parent_path() / path;
}
}
