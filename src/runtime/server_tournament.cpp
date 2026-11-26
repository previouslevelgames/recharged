#include "runtime/server_tournament.h"

#include "bootstrap/texts.h"
#include "platform/competitive_rules.h"
#include "platform/game_presence.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace mscharged::runtime
{
std::string TournamentInitials(const std::string& name)
{
    std::string initials;
    for (const char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) && initials.size() < 2)
            initials += char(std::toupper(static_cast<unsigned char>(c)));
    return initials.empty() ? "?" : initials;
}

namespace
{
using texts::T;

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

// The text of a tournament's format.
const char* FormatText(const std::string& format)
{
    return format == "double"       ? T("menus.tournament.server.double")
           : format == "single_ash" ? T("menus.tournament.server.single_ash")
                                    : T("menus.tournament.server.single");
}

// "Winners Round 2", "Losers Final", "Grand Final", "Ash Final".
std::string RoundText(const platform::TournamentPage& page, const platform::TournamentSet& set)
{
    // Single elimination, with an ash bracket or not.
    const bool single = page.tournament.format != "double";
    const std::string round = std::to_string(set.round);
    if (set.side == "ash")
        return set.round == LastRound(page, "ash") ? std::string(T("menus.tournament.server.ash_final"))
                                                   : T("menus.tournament.server.ash_round", {{"round", round}});
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
// its reset after them), the losers' (or the ash bracket's) below.
std::vector<ui::BracketSet> Bracket(const platform::TournamentPage& page, std::int64_t you, std::int64_t yours)
{
    std::vector<ui::BracketSet> bracket;
    const int winners = LastRound(page, "winners");
    for (const auto& set : page.sets)
    {
        if (set.state == "void" && set.side == "grand_final_reset") continue;
        ui::BracketSet cell;
        cell.id = std::to_string(set.id);
        cell.round = set.side == "losers" || set.side == "ash" ? -set.round
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
        int number = 0;
        for (const auto& played : set.played)
        {
            ui::BracketGame game;
            game.captains[0] = platform::CaptainIndex(played.home_captain);
            game.captains[1] = platform::CaptainIndex(played.away_captain);
            game.goals[0] = played.home_score;
            game.goals[1] = played.away_score;
            const char* home = platform::CaptainName(game.captains[0]);
            const char* away = platform::CaptainName(game.captains[1]);
            ++number;
            if (!home || !away) continue;
            game.text = T(played.interrupted ? "menus.bracket.game_interrupted" : "menus.bracket.game",
                          {{"game", std::to_string(number)},
                           {"home", home},
                           {"away", away},
                           {"home_score", std::to_string(played.home_score)},
                           {"away_score", std::to_string(played.away_score)}});
            cell.games.push_back(std::move(game));
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

// A tournament's schedule while it matters, by this computer's clock (`now`,
// Unix seconds): when its entries close (or that they did) while it is open
// for entries, when its games begin until they do. Empty: nothing to say.
std::string ScheduleText(const platform::ProfileTournament& t, std::int64_t now)
{
    std::string text;
    const auto add = [&](std::string part) {
        if (!text.empty()) text += "  ·  ";
        text += std::move(part);
    };
    const bool open = t.state == "registration";
    if (open && t.registration_ends_at > 0)
        add(t.registration_ends_at > now
                ? T("menus.tournaments.entries_close", {{"when", platform::DescribeChatTime(now, t.registration_ends_at)}})
                : std::string(T("menus.tournaments.entries_closed")));
    if ((open || t.state == "running") && t.starts_at > now)
        add(T("menus.tournaments.games_from", {{"when", platform::DescribeChatTime(now, t.starts_at)}}));
    return text;
}
} // namespace

ui::TournamentListModel ServerTournaments(const platform::TournamentFeed::Snapshot& feed, std::int64_t now)
{
    ui::TournamentListModel model;
    if (feed.tournaments.empty())
    {
        model.note = !feed.loaded            ? std::string(T("menus.tournament.server.loading"))
                     : !feed.error.empty()   ? T("menus.tournament.server.failed", {{"error", feed.error}})
                                             : std::string(T("menus.tournaments.none"));
        return model;
    }
    if (!feed.error.empty()) model.note = T("menus.tournament.server.stale", {{"error", feed.error}});
    const auto entry = [now](const platform::ProfileTournament& t) {
        ui::TournamentEntry entry;
        entry.key = std::to_string(t.id);
        entry.name = t.name;
        entry.initials = TournamentInitials(t.name);
        const bool open = t.state == "registration", running = t.state == "running";
        entry.kicker = open && t.joined      ? T("menus.tournaments.yours_registration")
                       : open                ? T("menus.tournaments.registration")
                       : running && t.joined ? T("menus.tournaments.yours_running")
                       : running             ? T("menus.tournaments.running")
                                             : T("menus.tournaments.finished");
        const std::string entrants =
            t.max_entrants > 0 ? T("menus.tournaments.entrants_max", {{"count", std::to_string(t.entrants)},
                                                                       {"max", std::to_string(t.max_entrants)}})
                               : texts::Count("menus.tournaments.entrants", t.entrants);
        entry.value = t.place > 0 ? T("menus.tournaments.place_short", {{"place", Place(t.place)}}) : entrants;
        entry.tone = t.joined && (open || running) ? ui::Tone::Good : ui::Tone::Neutral;
        entry.lines.push_back(T("menus.tournaments.format",
                                {{"format", FormatText(t.format)},
                                 {"rule_set", t.rule_set}}));
        // The semifinals only when they differ from the other sets.
        const bool semifinals = t.semifinals_best_of > 0 && t.semifinals_best_of != t.best_of;
        entry.lines.push_back(T(semifinals ? "menus.tournaments.best_of_semifinals" : "menus.tournaments.best_of",
                                {{"best_of", std::to_string(t.best_of)},
                                 {"semifinals", std::to_string(t.semifinals_best_of)},
                                 {"finals", std::to_string(t.finals_best_of)}}));
        if (auto schedule = ScheduleText(t, now); !schedule.empty()) entry.lines.push_back(std::move(schedule));
        entry.lines.push_back(entrants);
        if (t.place > 0) entry.lines.push_back(T("menus.tournaments.place", {{"place", Place(t.place)}}));
        return entry;
    };
    const auto featured = platform::ChooseTournament(feed.tournaments);
    if (featured) model.featured = entry(feed.tournaments[*featured]);
    for (std::size_t at = 0; at < feed.tournaments.size(); ++at)
    {
        const auto& t = feed.tournaments[at];
        if (at == featured || t.state == "cancelled") continue;
        (t.state == "finished" ? model.past : model.others).push_back(entry(t));
    }
    return model;
}

ui::TournamentModel ServerTournament(const platform::TournamentFeed::Snapshot& feed, std::int64_t shown)
{
    ui::TournamentModel model;
    model.games = 0;
    // The page asked for another than the feed's last answer: it comes.
    if (shown && feed.page && feed.page->tournament.id != shown)
    {
        model.note = T("menus.tournament.server.loading");
        return model;
    }
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
    model.event = std::string(FormatText(tournament.format)) + "  ·  " + tournament.rule_set;
    model.ash = tournament.format == "single_ash";
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
    model.opponent_initials = TournamentInitials(name);
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
namespace
{
// The player's set played by match `match` (the page asked after it was made).
const platform::TournamentSet* SetOfMatch(const platform::TournamentPage& page, std::int64_t match)
{
    if (!page.me || !match) return nullptr;
    const std::int64_t you = page.me->player;
    for (const auto& candidate : page.sets)
        if (candidate.match == match && (candidate.home == you || candidate.away == you)) return &candidate;
    return nullptr;
}

// UTF-8 as the game's panel shows it: the basic plane, at most 127 units
// (characters past it and invalid bytes left out).
std::u16string PanelText(std::string_view text)
{
    std::u16string shown;
    for (std::size_t n = 0; n < text.size() && shown.size() < 127;)
    {
        const auto lead = static_cast<unsigned char>(text[n]);
        const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3
                                 : (lead & 0xF8) == 0xF0                   ? 4
                                                                           : 1;
        char32_t c = length == 1 ? lead : lead & (0x7F >> length);
        bool valid = (length == 1 && lead < 0x80) || length > 1;
        for (std::size_t k = 1; k < length && valid; ++k)
        {
            const auto next = n + k < text.size() ? static_cast<unsigned char>(text[n + k]) : 0;
            valid = (next & 0xC0) == 0x80;
            c = (c << 6) | (next & 0x3F);
        }
        n += length;
        if (valid && c >= 0x20 && c <= 0xFFFF && !(c >= 0xD800 && c < 0xE000)) shown += char16_t(c);
    }
    return shown;
}
} // namespace

TournamentCall TournamentCallOf(const platform::TournamentFeed::Snapshot& feed)
{
    TournamentCall call;
    if (!feed.signed_in || !feed.own || !feed.own->me) return call;
    const auto& page = *feed.own;
    const auto& me = *page.me;
    if (page.tournament.state != "running" || !me.entrant || me.dropped || !me.eligible || !me.set) return call;
    const auto* set = page.Set(me.set);
    if (!set || !set->home || !set->away) return call;
    const std::int64_t opponent = set->home == me.player ? set->away : set->home;
    call.opponent = NameOf(page, opponent);
    call.tournament = page.tournament.name;
    const int played = std::max(set->home_wins, 0) + std::max(set->away_wins, 0);
    if (set->state == "ready")
    {
        call.kind = TournamentCall::Kind::SetReady;
        call.key = "ready:" + std::to_string(set->id) + ":" + std::to_string(played);
    }
    else if (set->state == "playing" && feed.match && feed.match->id == set->match && feed.match->opponent_ready
             && !feed.match->you_ready)
    {
        call.kind = TournamentCall::Kind::OpponentReady;
        call.key = "opponent:" + std::to_string(set->match);
    }
    return call;
}

TournamentCeremony TournamentCeremonyOf(const platform::TournamentPage& page, std::int64_t match)
{
    TournamentCeremony ceremony;
    const auto* set = SetOfMatch(page, match);
    if (!set) return ceremony;
    const std::int64_t you = page.me->player;
    const std::int64_t opponent = set->home == you ? set->away : set->home;
    ceremony.title = PanelText(T("menus.tournament.server.ceremony_title", {{"tournament", page.tournament.name}}));
    ceremony.own = PanelText(T("menus.tournament.server.ceremony_winner", {{"name", NameOf(page, you)}}));
    ceremony.opponent = PanelText(T("menus.tournament.server.ceremony_winner", {{"name", NameOf(page, opponent)}}));
    return ceremony;
}

std::optional<ChargedSetResume> TournamentResumeOf(const platform::ProfileMatch& match)
{
    const auto& found = match.tournament.resume;
    if (!found) return std::nullopt;
    ChargedSetResume resume{};
    const auto team = [](const platform::TournamentResume::Team& from, int& captain, int (&sidekicks)[3]) {
        captain = platform::CaptainIndex(from.captain);
        if (captain < 0 || from.sidekicks.size() != 3) return false;
        for (int slot = 0; slot < 3; ++slot)
            if ((sidekicks[slot] = platform::SidekickIndex(from.sidekicks[std::size_t(slot)])) < 0) return false;
        return true;
    };
    if (!team(found->you, resume.own_captain, resume.own_sidekicks)
        || !team(found->opponent, resume.opponent_captain, resume.opponent_sidekicks))
        return std::nullopt;
    resume.own_score = found->you.score;
    resume.opponent_score = found->opponent.score;
    resume.seconds = float(std::max(found->seconds, 0));
    return resume;
}

int TournamentSideOf(const platform::TournamentPage& page, std::int64_t match)
{
    const auto* set = SetOfMatch(page, match);
    if (!set || !set->game_home || !set->home || !set->away) return -1;
    if (set->game_home != set->home && set->game_home != set->away) return -1;
    return set->game_home == page.me->player ? 0 : 1;
}

TournamentEnding TournamentEndingOf(const platform::TournamentPage& page, std::int64_t match)
{
    TournamentEnding ending;
    const platform::TournamentSet* set = SetOfMatch(page, match);
    if (!set || set->games <= 0) return ending;
    const std::int64_t you = page.me->player;
    const bool home = set->home == you;
    // The set ends the tournament when the player in its home place (true)
    // or away place wins it.
    const auto ends = [&](bool home_wins) {
        if (set->side == "grand_final_reset") return true;
        if (set->side == "grand_final") return home_wins; // home: from the winners' bracket
        // Single elimination's final (an ash bracket's ends nothing).
        return page.tournament.format != "double" && set->side == "winners"
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
