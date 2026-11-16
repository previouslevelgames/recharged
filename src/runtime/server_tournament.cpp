#include "runtime/server_tournament.h"

#include "bootstrap/texts.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace mscharged::runtime
{
namespace
{
using texts::T;

// Up to two letters of a name for its picture.
std::string Initials(const std::string& name)
{
    std::string initials;
    for (const char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) && initials.size() < 2)
            initials += char(std::toupper(static_cast<unsigned char>(c)));
    return initials.empty() ? "?" : initials;
}

std::string NameOf(const platform::TournamentPage& page, std::int64_t player)
{
    if (const auto* entrant = page.Entrant(player)) return entrant->player.name;
    return {};
}

int LastRound(const platform::TournamentPage& page, const std::string& side)
{
    int last = 0;
    for (const auto& set : page.sets)
        if (set.side == side) last = std::max(last, set.round);
    return last;
}

// "Winners Round 2", "Losers Final", "Grand Final".
std::string RoundText(const platform::TournamentPage& page, const platform::TournamentSet& set)
{
    const bool single = page.tournament.format == "single";
    const std::string round = std::to_string(set.round);
    if (set.side == "grand_final") return T("menus.tournament.server.grand_final");
    if (set.side == "grand_final_reset") return T("menus.tournament.server.grand_final_reset");
    if (set.side == "losers")
        return set.round == LastRound(page, "losers") ? std::string(T("menus.tournament.server.losers_final"))
                                                      : T("menus.tournament.server.losers_round", {{"round", round}});
    if (set.round == LastRound(page, "winners"))
        return single ? T("menus.tournament.server.final") : T("menus.tournament.server.winners_final");
    return single ? T("menus.tournament.server.round", {{"round", round}})
                  : T("menus.tournament.server.winners_round", {{"round", round}});
}

ui::BracketSetState BracketState(const std::string& state)
{
    if (state == "finished") return ui::BracketSetState::Done;
    if (state == "playing" || state == "disputed") return ui::BracketSetState::Going;
    return ui::BracketSetState::Waiting;
}

// The whole bracket, in its order: the winners' rounds (the grand final and
// its reset after them), the losers' below.
std::vector<ui::BracketSet> Bracket(const platform::TournamentPage& page, std::int64_t you, std::int64_t yours)
{
    std::vector<ui::BracketSet> bracket;
    const int winners = LastRound(page, "winners");
    for (const auto& set : page.sets)
    {
        if (set.state == "void" && set.side == "grand_final_reset") continue;
        ui::BracketSet cell;
        cell.id = std::to_string(set.id);
        cell.round = set.side == "losers"              ? -set.round
                     : set.side == "grand_final"       ? winners + 1
                     : set.side == "grand_final_reset" ? winners + 2
                                                       : set.round;
        cell.round_text = RoundText(page, set);
        cell.state = BracketState(set.state);
        cell.yours = set.id == yours;
        const std::int64_t players[2] = {set.home, set.away};
        const int wins[2] = {set.home_wins, set.away_wins};
        for (int n = 0; n < 2; ++n)
        {
            auto& slot = cell.slots[n];
            slot.name = players[n] ? NameOf(page, players[n]) : std::string();
            if (wins[n] >= 0) slot.score = std::to_string(wins[n]);
            slot.winner = players[n] && players[n] == set.winner;
            slot.you = players[n] && players[n] == you;
            // A line only from the set whose winner comes here.
            if (set.from[n].kind == platform::TournamentFrom::Kind::Winner)
                cell.prereq[n] = std::to_string(set.from[n].value);
        }
        bracket.push_back(std::move(cell));
    }
    return bracket;
}

std::string Place(int place)
{
    const int last = place % 10, tens = place % 100;
    const char* suffix = (tens >= 11 && tens <= 13) ? "th"
                         : last == 1               ? "st"
                         : last == 2               ? "nd"
                         : last == 3               ? "rd"
                                                   : "th";
    return std::to_string(place) + suffix;
}
} // namespace

ui::TournamentModel ServerTournament(const platform::TournamentFeed::Snapshot& feed)
{
    ui::TournamentModel model;
    model.games = 0;
    if (!feed.page)
    {
        model.name = T("menus.tournament.title");
        model.note = !feed.loaded            ? std::string(T("menus.tournament.server.loading"))
                     : !feed.error.empty()   ? T("menus.tournament.server.failed", {{"error", feed.error}})
                                             : std::string(T("menus.tournament.server.none"));
        return model;
    }
    const auto& page = *feed.page;
    const auto& tournament = page.tournament;
    model.name = tournament.name;
    model.event = std::string(T(tournament.format == "double" ? "menus.tournament.server.double"
                                                              : "menus.tournament.server.single"))
        + "  ·  " + tournament.rule_set;
    const auto me = page.me;
    const std::int64_t you = me ? me->player : 0;
    const platform::TournamentSet* set = me && me->set ? page.Set(me->set) : nullptr;
    model.bracket = Bracket(page, you, set ? set->id : 0);
    auto note = [&](std::string text) {
        if (!model.note.empty()) model.note += "  ";
        model.note += std::move(text);
    };
    if (!feed.error.empty()) note(T("menus.tournament.server.stale", {{"error", feed.error}}));
    if (!feed.action_error.empty()) note(feed.action_error);
    const int count = tournament.entrants;
    if (!feed.signed_in || !me)
    {
        note(T("menus.tournament.server.signed_out"));
        return model;
    }
    if (!me->eligible)
    {
        note(T("menus.tournament.server.discord_required"));
        return model;
    }
    const bool full = tournament.max_entrants > 0 && count >= tournament.max_entrants;
    if (tournament.state == "registration")
    {
        model.can_join = !me->entrant && !full && !feed.busy;
        model.can_leave = me->entrant && !feed.busy;
        note(me->entrant ? T("menus.tournament.server.joined", {{"count", std::to_string(count)}})
             : full      ? std::string(T("menus.tournament.server.full"))
                         : T("menus.tournament.server.entries_open", {{"count", std::to_string(count)}}));
        return model;
    }
    if (tournament.state == "cancelled")
    {
        note(T("menus.tournament.server.cancelled"));
        return model;
    }
    if (tournament.state == "finished")
    {
        note(me->place > 0 ? T("menus.tournament.server.finished_place", {{"place", Place(me->place)}})
                           : std::string(T("menus.tournament.server.finished")));
        return model;
    }
    if (!me->entrant)
    {
        note(T("menus.tournament.server.not_in"));
        return model;
    }
    if (me->dropped)
    {
        note(T("menus.tournament.server.left"));
        return model;
    }
    if (!set)
    {
        note(T("menus.tournament.server.out"));
        return model;
    }
    const std::int64_t opponent = set->home == you ? set->away : set->home;
    model.has_set = true;
    model.games = set->games;
    model.round = RoundText(page, *set);
    model.opponent_known = opponent != 0 && (set->state == "ready" || set->state == "playing") && !feed.busy;
    const std::string name = opponent ? NameOf(page, opponent) : std::string();
    model.opponent = opponent ? name : std::string(T("menus.tournament.opponent_waiting"));
    model.opponent_initials = opponent ? Initials(name) : "?";
    if (set->home_wins >= 0 && set->away_wins >= 0)
        model.score = set->home == you ? std::to_string(set->home_wins) + " - " + std::to_string(set->away_wins)
                                       : std::to_string(set->away_wins) + " - " + std::to_string(set->home_wins);
    if (set->state == "pending") note(T("menus.tournament.server.waiting_set"));
    else if (set->state == "disputed")
    {
        model.opponent_known = false;
        note(T("menus.tournament.server.disputed"));
    }
    else if (feed.match && set->state == "playing" && feed.match->id == set->match)
    {
        model.you_ready = feed.match->you_ready;
        if (feed.match->you_ready && !feed.match->opponent_ready)
            note(T("menus.tournament.server.you_ready", {{"name", name}}));
        else if (!feed.match->you_ready && feed.match->opponent_ready)
            note(T("menus.tournament.server.opponent_ready", {{"name", name}}));
    }
    return model;
}
TournamentEnding TournamentEndingOf(const platform::TournamentPage& page, std::int64_t match)
{
    TournamentEnding ending;
    if (!page.me || !match) return ending;
    const std::int64_t you = page.me->player;
    const platform::TournamentSet* set = nullptr;
    for (const auto& candidate : page.sets)
        if (candidate.match == match && (candidate.home == you || candidate.away == you)) set = &candidate;
    if (!set || set->games <= 0) return ending;
    const bool home = set->home == you;
    // The set ends the tournament when the player in its home place (true)
    // or away place wins it.
    const auto ends = [&](bool home_wins) {
        if (set->side == "grand_final_reset") return true;
        if (set->side == "grand_final") return home_wins; // home: from the winners' bracket
        return page.tournament.format == "single" && set->side == "winners"
            && set->round == LastRound(page, "winners");
    };
    const int needed = set->games / 2 + 1;
    const int home_wins = std::max(set->home_wins, 0), away_wins = std::max(set->away_wins, 0);
    const bool home_takes = home_wins + 1 >= needed && ends(true);
    const bool away_takes = away_wins + 1 >= needed && ends(false);
    ending.own = home ? home_takes : away_takes;
    ending.opponent = home ? away_takes : home_takes;
    return ending;
}
} // namespace mscharged::runtime
