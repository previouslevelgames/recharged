#pragma once

#include "platform/discord_ipc.h"
#include "platform/presence_snapshot.h"

#include <cstdint>
#include <string>

namespace mscharged::platform
{
// The game's English names for its captains (eTeamID) and stadiums
// (eStadiumID); nullptr for an unknown number.
const char* CaptainName(int team);
const char* StadiumName(int stadium);

// A set of competitive play (the launcher's sets) this game plays: shown in
// place of the match mode, with the games won and lost so far when the
// series has more than one.
struct SetText
{
    std::string opponent;
    int won = 0, lost = 0, games = 0;
};

// Turns what the game is doing into the Discord status (issue #71): the menu
// or the match (mode, stadium, captains, score) and how long the match has
// run. Keeps the match timer steady: Discord counts it on by itself, so it
// moves only when the game's clock has drifted (stopped after a goal).
class GamePresence
{
public:
    // `game_title` and `region` describe the disc ("Mario Strikers Charged",
    // "USA", "R4QE01"); `session_start` is when the game started (Unix time),
    // the timer shown in the menus.
    GamePresence(std::string game_title, std::string region, std::string game_id, std::int64_t session_start);

    // The status for this snapshot at Unix time `now`; `set` while the game
    // plays a set.
    discord::Activity Describe(const ChargedPresenceSnapshot& snapshot, std::int64_t now,
                               const SetText* set = nullptr);

private:
    std::string game_line_;   // "Mario Strikers Charged · USA"
    std::string disc_line_;   // "Mario Strikers Charged · USA (R4QE01)"
    std::int64_t session_start_;
    std::int64_t match_start_ = 0;
};
}
