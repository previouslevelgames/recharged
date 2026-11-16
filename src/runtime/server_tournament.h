#pragma once

#include "platform/tournament_feed.h"
#include "ui/rui_pages.h"

namespace mscharged::runtime
{
// The TOURNAMENT page's model from the profile server's tournament (the
// feed's last answers): the tournament, the player's set in it, whether
// they can join or leave, and a note while it loads, when the server fails,
// when they need Discord, before the start and after their last set.
ui::TournamentModel ServerTournament(const platform::TournamentFeed::Snapshot& feed);

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
} // namespace mscharged::runtime
