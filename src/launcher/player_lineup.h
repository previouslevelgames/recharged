#pragma once

#include "platform/wiimote_ids.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

// Who plays if the game starts now, as the launcher shows it. It follows the
// game at startup (InitializeNativeHardwareInput, platform/hardware_owner.cpp):
// controllers that are not connected drop out and the others close up in
// order; with nobody left keyboard & mouse is player 1. Independent of
// SDL/ImGui so it can be unit tested.
namespace mscharged::launcher
{
struct PlayerLineup
{
    std::array<bool, 4> missing{};    // this player's controller is not connected
    std::vector<std::string> playing; // the devices that play, in order
};

// players: controls.player1-4. remote_connected(id): whether that Wii Remote
// answers (true while nobody has checked yet). gamepads: standard gamepads
// connected, which the game numbers gamepad1-4 in that order.
template <typename RemoteConnected>
PlayerLineup PredictLineup(const std::array<std::string, 4>& players, RemoteConnected remote_connected, int gamepads)
{
    PlayerLineup lineup;
    // Gamepad players keep their places in gamepad order while enough are connected.
    std::array<int, 4> pad_player{-1, -1, -1, -1};
    for (int n = 0; n < 4; ++n)
    {
        const auto& device = players[std::size_t(n)];
        if (device.size() == 8 && device.compare(0, 7, "gamepad") == 0 && device[7] >= '1' && device[7] <= '4')
            pad_player[std::size_t(device[7] - '1')] = n;
    }
    for (const int n : pad_player)
    {
        if (n < 0) continue;
        if (gamepads > 0) --gamepads;
        else lineup.missing[std::size_t(n)] = true;
    }
    bool dropped = false;
    for (std::size_t n = 0; n < 4; ++n)
    {
        if (platform::IsWiimoteId(players[n]) && !remote_connected(players[n])) lineup.missing[n] = true;
        dropped |= lineup.missing[n];
    }
    for (std::size_t n = 0; n < 4; ++n)
        if (players[n] != "off" && !lineup.missing[n]) lineup.playing.push_back(players[n]);
    if (dropped && lineup.playing.empty()) lineup.playing.push_back("keyboard");
    return lineup;
}

// Every controller but keyboard & mouse missing: the longest note there can be.
inline PlayerLineup WorstLineup(const std::array<std::string, 4>& players)
{
    return PredictLineup(players, [](const std::string&) { return false; }, 0);
}

inline std::string ShortDeviceName(const std::string& device)
{
    if (device == "keyboard") return "keyboard & mouse";
    if (device.compare(0, 7, "gamepad") == 0) return "gamepad " + device.substr(7);
    return "Wii Remote";
}

// "a", "a and b", "a, b and c".
inline std::string JoinList(const std::vector<std::string>& items)
{
    std::string text;
    for (std::size_t i = 0; i < items.size(); ++i)
        text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
    return text;
}

// "Player 1's Wii Remote is not connected, so the game starts with keyboard &
// mouse as player 1.", or empty while every controller is there.
inline std::string MissingControllersNote(const std::array<std::string, 4>& players, const PlayerLineup& lineup)
{
    std::vector<std::string> missing;
    for (std::size_t n = 0; n < 4; ++n)
        if (lineup.missing[n]) missing.push_back("player " + std::to_string(n + 1) + "'s " + ShortDeviceName(players[n]));
    if (missing.empty()) return {};
    std::string note = JoinList(missing);
    note[0] = 'P';
    note += missing.size() == 1 ? " is not connected" : " are not connected";
    note += ", so the game starts with ";
    if (lineup.playing.size() == 1) return note + ShortDeviceName(lineup.playing[0]) + " as player 1.";
    std::vector<std::string> names;
    for (const auto& device : lineup.playing) names.push_back(ShortDeviceName(device));
    return note + std::to_string(names.size()) + " players: " + JoinList(names) + ".";
}

// The same in a few words, for the Play page: what is not connected ("Wii
// Remote not connected"; "Player 2's Wii Remote not connected" when more than
// one player is set; "2 controllers not connected") and who plays instead
// ("keyboard & mouse plays", "... plays alone", "2 players start"). Empty
// while every controller is there.
struct ControllersBrief
{
    std::string missing;
    std::string instead;
};

inline ControllersBrief MissingControllersBrief(const std::array<std::string, 4>& players, const PlayerLineup& lineup)
{
    std::size_t set = 0, count = 0, first = 0;
    for (std::size_t n = 0; n < 4; ++n)
    {
        set += players[n] != "off";
        if (lineup.missing[n] && count++ == 0) first = n;
    }
    if (count == 0) return {};
    ControllersBrief brief;
    if (count > 1) brief.missing = std::to_string(count) + " controllers not connected";
    else
    {
        brief.missing = (set > 1 ? "player " + std::to_string(first + 1) + "'s " : std::string())
            + ShortDeviceName(players[first]) + " not connected";
        if (brief.missing[0] >= 'a' && brief.missing[0] <= 'z') brief.missing[0] = char(brief.missing[0] - 'a' + 'A');
    }
    brief.instead = lineup.playing.size() == 1
        ? ShortDeviceName(lineup.playing[0]) + (set > 1 ? " plays alone" : " plays")
        : std::to_string(lineup.playing.size()) + " players start";
    return brief;
}
} // namespace mscharged::launcher
