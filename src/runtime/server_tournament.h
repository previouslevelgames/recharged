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
} // namespace mscharged::runtime
