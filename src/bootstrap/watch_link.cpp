#include "bootstrap/watch_link.h"

#include "platform/path.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <vector>

namespace mscharged
{
namespace
{
constexpr std::string_view kScheme = "recharged:";

bool All(std::string_view text, int (*is)(int))
{
    return !text.empty()
        && std::all_of(text.begin(), text.end(), [is](char c) { return is(static_cast<unsigned char>(c)) != 0; });
}

int IsLowerHex(int c)
{
    return std::isdigit(c) || (c >= 'a' && c <= 'f');
}
} // namespace

bool IsWatchLink(std::string_view text)
{
    if (text.size() < kScheme.size()) return false;
    for (std::size_t n = 0; n < kScheme.size(); ++n)
        if (std::tolower(static_cast<unsigned char>(text[n])) != kScheme[n]) return false;
    return true;
}

std::filesystem::path WriteWatchLinkTicket(std::string_view link, const std::filesystem::path& folder,
                                           std::string& error)
{
    error = "The watch link does not name a match.";
    if (!IsWatchLink(link) || link.size() > 1024) return {};
    std::string_view rest = link.substr(kScheme.size());
    while (!rest.empty() && rest.front() == '/') rest.remove_prefix(1);
    // watch/<match>, then the query (a browser may add a slash before it).
    const auto query_at = rest.find('?');
    std::string_view path = rest.substr(0, query_at);
    const std::string_view query = query_at == std::string_view::npos ? std::string_view() : rest.substr(query_at + 1);
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    // watch/<match>: as it is played; replay/<match>: a game the server kept.
    const bool kept = path.substr(0, 7) == "replay/";
    if (path.substr(0, 6) != "watch/" && !kept) return {};
    const std::string_view match = path.substr(kept ? 7 : 6);
    if (!All(match, [](int c) { return std::isdigit(c); }) || match.size() > 18 || match.front() == '0') return {};
    std::string_view ticket, release, game;
    for (std::string_view parameters = query; !parameters.empty();)
    {
        const auto end = parameters.find('&');
        const std::string_view parameter = parameters.substr(0, end);
        parameters = end == std::string_view::npos ? std::string_view() : parameters.substr(end + 1);
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view name = parameter.substr(0, equals), value = parameter.substr(equals + 1);
        if (name == "ticket") ticket = value;
        else if (name == "release") release = value;
        else if (name == "game") game = value;
    }
    if (!All(ticket, IsLowerHex) || ticket.size() > 256 || release.size() != 4
        || !All(release, [](int c) { return std::isalnum(c); }))
        return {};
    if (kept && (game.size() != 1 || game[0] < '1' || game[0] > '9')) return {};

    // The dashboard's ticket file: "MSCRPLY" 0x1A, version 2, the header's
    // size, the header (JSON; every character above needs no escaping).
    const std::string header = "{\"match\": " + std::string(match)
        + (kept ? ", \"game\": " + std::string(game) : std::string()) + ", \"ticket\": \"" + std::string(ticket)
        + "\", \"release\": \"" + std::string(release) + "\"}";
    std::vector<char> file = {'M', 'S', 'C', 'R', 'P', 'L', 'Y', 0x1A, 2, 0};
    file.push_back(char(header.size() & 0xFF));
    file.push_back(char(header.size() >> 8));
    file.insert(file.end(), header.begin(), header.end());

    std::error_code failure;
    std::filesystem::create_directories(folder, failure);
    const auto target = folder
        / ("watch-match-" + std::string(match) + (kept ? "-game-" + std::string(game) : std::string()) + ".msc");
    auto partial = target;
    partial += ".tmp";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(file.data(), std::streamsize(file.size()));
        if (!out)
        {
            error = "The watch ticket could not be written to " + PathUtf8(folder) + ".";
            return {};
        }
    }
    std::filesystem::rename(partial, target, failure);
    if (failure)
    {
        std::filesystem::remove(partial, failure);
        error = "The watch ticket could not be written to " + PathUtf8(folder) + ".";
        return {};
    }
    error.clear();
    return target;
}
} // namespace mscharged
