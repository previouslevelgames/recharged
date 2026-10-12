#include "launcher/player_lineup.h"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace mscharged::launcher;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void RequireNote(const std::string& note, const std::string& expected)
{
    Require(note == expected, "Note \"" + note + "\" is not \"" + expected + "\"");
}

const std::string kRemote = "remote-9ce635c69076";
const std::string kOtherRemote = "remote-0019fd1a2b3c";
auto Connected(bool value) { return [value](const std::string&) { return value; }; }
} // namespace

int main()
{
    try
    {
        // A single player whose Wii Remote is off: keyboard & mouse plays.
        std::array<std::string, 4> players{kRemote, "off", "off", "off"};
        auto lineup = PredictLineup(players, Connected(false), 0);
        Require(lineup.missing[0] && lineup.playing.size() == 1 && lineup.playing[0] == "keyboard",
                "A missing remote leaves keyboard & mouse as player 1");
        RequireNote(MissingControllersNote(players, lineup),
                    "Player 1's Wii Remote is not connected, so the game starts with keyboard & mouse as player 1.");
        // Not checked yet (or connected): nothing is missing and nothing is said.
        lineup = PredictLineup(players, Connected(true), 0);
        Require(!lineup.missing[0] && lineup.playing.size() == 1 && lineup.playing[0] == kRemote, "Connected remote plays");
        RequireNote(MissingControllersNote(players, lineup), "");

        // Player 2's DolphinBar remote is missing: player 1 plays alone.
        players = {"keyboard", "remote2", "off", "off"};
        lineup = PredictLineup(players, Connected(false), 0);
        Require(!lineup.missing[0] && lineup.missing[1] && lineup.playing.size() == 1, "Player 2 drops out");
        RequireNote(MissingControllersNote(players, lineup),
                    "Player 2's Wii Remote is not connected, so the game starts with keyboard & mouse as player 1.");

        // Gamepads stay in gamepad order, as in the game: with one connected,
        // gamepad 1 plays (as player 1) and player 1's gamepad 2 drops out.
        players = {"gamepad2", "gamepad1", "off", "off"};
        lineup = PredictLineup(players, Connected(true), 1);
        Require(lineup.missing[0] && !lineup.missing[1] && lineup.playing.size() == 1 && lineup.playing[0] == "gamepad1",
                "Gamepad players keep gamepad order");
        RequireNote(MissingControllersNote(players, lineup),
                    "Player 1's gamepad 2 is not connected, so the game starts with gamepad 1 as player 1.");
        Require(!PredictLineup(players, Connected(true), 2).missing[0], "Two gamepads: both play");

        // Two missing, two left: they close up in order.
        players = {kRemote, "keyboard", kOtherRemote, "gamepad1"};
        lineup = PredictLineup(players, [](const std::string& id) { return id == kOtherRemote; }, 0);
        Require(lineup.missing[0] && !lineup.missing[1] && !lineup.missing[2] && lineup.missing[3]
                    && lineup.playing.size() == 2 && lineup.playing[0] == "keyboard" && lineup.playing[1] == kOtherRemote,
                "The others close up in order");
        RequireNote(MissingControllersNote(players, lineup),
                    "Player 1's Wii Remote and player 4's gamepad 1 are not connected, so the game starts with 2 "
                    "players: keyboard & mouse and Wii Remote.");

        // The longest note: every controller but keyboard & mouse missing.
        players = {kRemote, "gamepad1", "keyboard", "off"};
        lineup = WorstLineup(players);
        Require(lineup.missing[0] && lineup.missing[1] && !lineup.missing[2] && lineup.playing.size() == 1,
                "Worst case keeps only keyboard & mouse");
        RequireNote(MissingControllersNote(players, lineup),
                    "Player 1's Wii Remote and player 2's gamepad 1 are not connected, so the game starts with "
                    "keyboard & mouse as player 1.");
        RequireNote(MissingControllersNote({"keyboard", "off", "off", "off"}, WorstLineup({"keyboard", "off", "off", "off"})),
                    "");

        Require(JoinList({"a"}) == "a" && JoinList({"a", "b"}) == "a and b" && JoinList({"a", "b", "c"}) == "a, b and c",
                "Lists read naturally");
        std::puts("Launcher player lineup passed (missing remotes and gamepads, gamepad order, close-up, notes).");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
