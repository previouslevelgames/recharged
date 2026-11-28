#pragma once

#include "platform/competitive_rules.h"
#include "platform/tournament_feed.h"
#include "ui/rui_pages.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mscharged::runtime
{
// Up to two letters of a name for the opponent's picture (in capitals), "?"
// for none.
std::string TournamentInitials(const std::string& name);

// The TOURNAMENT page's model from the profile server's tournament (the
// feed's last answers): the tournament, the player's set in it, whether
// they can join or leave, and a note while it loads, when the server fails,
// when they need Discord, before the start and after their last set.
// `shown`: the tournament the page asked for (0: the feed's choice), a
// note while the feed's answer is another's. `now`: this computer's clock (Unix
// seconds), for the schedule: no JOIN once entries closed, no READY before
// the games begin.
ui::TournamentModel ServerTournament(const platform::TournamentFeed::Snapshot& feed, std::int64_t shown = 0,
                                     std::int64_t now = 0);
// The TOURNAMENTS page's: the player's own tournament (else the newest) in
// the big card, the others open for entries or under way, the last that
// ended (none called off). `now`: this computer's clock (Unix seconds), for
// the schedule.
ui::TournamentListModel ServerTournaments(const platform::TournamentFeed::Snapshot& feed, std::int64_t now = 0);

// Whether winning the game about to be played in match `match` (the
// player's set's) ends the tournament: for this player (own), for the
// opponent. In single elimination the final; in double the grand final's
// reset, and the grand final itself only for the player from the winners'
// bracket (else the reset is played); and only the game that gives the set.
struct TournamentEnding
{
    bool own = false, opponent = false;
};
TournamentEnding TournamentEndingOf(const platform::TournamentPage& page, std::int64_t match);

// What the player should hear about their set from the feed's last answer
// (their own tournament's page): a game of it to play (ready, the opponent
// known), or the opponent READY while they are not. `key` names that moment
// (the set and its games played, or the match): the host tells it once.
// Nothing before the tournament's games begin by `now` (this computer's
// clock, Unix seconds).
struct TournamentCall
{
    enum class Kind { None, SetReady, OpponentReady } kind = Kind::None;
    std::string key, opponent, tournament;
};
TournamentCall TournamentCallOf(const platform::TournamentFeed::Snapshot& feed, std::int64_t now = 0);

// The ceremony's panel for that game (UTF-16 of the basic plane, at most 127
// units each): its title, and who won, this player or the opponent.
struct TournamentCeremony
{
    std::u16string title, own, opponent;
};
TournamentCeremony TournamentCeremonyOf(const platform::TournamentPage& page, std::int64_t match);

// The game the match `match` goes on from (its tournament context's resume)
// for the game module: none without one, or with a team unknown.
std::optional<ChargedSetResume> TournamentResumeOf(const platform::ProfileMatch& match);

// This player's side in the game of match `match` by the set's turn (the
// server's game_home): 0 home, 1 away, -1 none (the game's own sides).
int TournamentSideOf(const platform::TournamentPage& page, std::int64_t match);
} // namespace mscharged::runtime
