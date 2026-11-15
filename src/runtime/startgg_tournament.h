#pragma once

#include "platform/competitive_rules.h"
#include "platform/startgg_feed.h"
#include "ui/rui_pages.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mscharged::runtime
{
// The TOURNAMENT page's model from the start.gg feed's last answers: the
// event, the player's next set in it (`user`: their start.gg user id), and
// a note while it loads, when start.gg fails, and when the player has no
// set to play.
ui::TournamentModel StartggTournament(const platform::StartggFeed::Snapshot& feed, std::string_view user);

// READY: the player's next set, to play now as a RANKED set against its
// opponent's WFC profile (`players`, [startgg] players: start.gg user to WFC
// profile, until the profile service links them). Without one, `error`
// says why, for the log.
struct StartggPlay
{
    std::string set;      // start.gg's set id
    std::string opponent; // as the event shows them
    std::optional<ChargedCompetitiveRules> rules;
    std::string error;
};
StartggPlay StartggSetToPlay(const platform::StartggFeed::Snapshot& feed, std::string_view user,
                             const std::vector<std::pair<std::string, unsigned>>& players);
} // namespace mscharged::runtime
