#pragma once

#include "platform/startgg_feed.h"
#include "ui/rui_pages.h"

#include <string_view>

namespace mscharged::runtime
{
// The TOURNAMENT page's model from the start.gg feed's last answers: the
// event, the player's next set in it (`user`: their start.gg user id), and
// a note while it loads, when start.gg fails, and when the player has no
// set to play.
ui::TournamentModel StartggTournament(const platform::StartggFeed::Snapshot& feed, std::string_view user);
} // namespace mscharged::runtime
