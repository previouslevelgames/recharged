// The profile server's tournament as the trophy room's pages show it
// (src/runtime/server_tournament.cpp): the TOURNAMENTS list, and what the
// player hears about their set.
#include "runtime/server_tournament.h"

#include <cstdio>

namespace
{
int failures = 0;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                       \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (false)

using namespace mscharged;
using Snapshot = platform::TournamentFeed::Snapshot;

platform::ProfileTournament Listed(std::int64_t id, const char* name, const char* state, bool joined, int place = 0)
{
    platform::ProfileTournament t;
    t.id = id;
    t.name = name;
    t.format = "double";
    t.state = state;
    t.rule_set = "Competitive";
    t.best_of = 3;
    t.finals_best_of = 5;
    t.entrants = 3;
    t.max_entrants = 8;
    t.joined = joined;
    t.place = place;
    return t;
}

// The player's own tournament in the big card, the others open or under
// way, the last that ended; none called off.
void List()
{
    Snapshot feed;
    feed.loaded = true;
    CHECK(runtime::ServerTournaments(feed).note == "No tournaments right now: an organizer opens them on the Recharged server.");
    feed.tournaments = {Listed(9, "New Cup", "registration", false), Listed(8, "My Cup", "running", true),
                        Listed(7, "Called Off", "cancelled", true), Listed(6, "Old Cup", "finished", true, 2)};
    const auto model = runtime::ServerTournaments(feed);
    CHECK(model.note.empty());
    CHECK(model.featured && model.featured->key == "8" && model.featured->kicker == "YOUR TOURNAMENT"
          && model.featured->tone == ui::Tone::Good);
    CHECK(model.featured && model.featured->lines.size() == 3
          && model.featured->lines[0] == "Double elimination  ·  Competitive"
          && model.featured->lines[1] == "Sets best of 3, finals best of 5" && model.featured->lines[2] == "3/8 players");
    CHECK(model.others.size() == 1 && model.others[0].key == "9" && model.others[0].kicker == "OPEN FOR ENTRIES"
          && model.others[0].value == "3/8 players");
    CHECK(model.past.size() == 1 && model.past[0].key == "6" && model.past[0].value == "2nd"
          && model.past[0].lines.back() == "You finished 2nd");
    // Nobody's: the newest open for entries.
    feed.tournaments[1].joined = false;
    CHECK(runtime::ServerTournaments(feed).featured->key == "9");
    // No cap on entries: the players alone.
    feed.tournaments[0].max_entrants = 0;
    feed.tournaments[0].entrants = 1;
    CHECK(runtime::ServerTournaments(feed).featured->value == "1 player");
    feed.tournaments[0].entrants = 2;
    CHECK(runtime::ServerTournaments(feed).featured->value == "2 players");
}

Snapshot Running(const char* state, int home_wins, int away_wins)
{
    Snapshot feed;
    feed.loaded = feed.signed_in = true;
    platform::TournamentPage page;
    page.tournament = Listed(4, "Recharged Cup", "running", true);
    platform::TournamentEntrant mario, bowser;
    mario.player.id = 7;
    mario.player.name = "Mario";
    bowser.player.id = 8;
    bowser.player.name = "Bowser";
    page.entrants = {mario, bowser};
    platform::TournamentSet set;
    set.id = 30;
    set.state = state;
    set.home = 7;
    set.away = 8;
    set.games = 3;
    set.home_wins = home_wins;
    set.away_wins = away_wins;
    set.match = 12;
    page.sets = {set};
    platform::TournamentMe me;
    me.player = 7;
    me.entrant = me.eligible = true;
    me.set = 30;
    page.me = me;
    feed.page = feed.own = page;
    return feed;
}

// A game of the player's set to play, once each; the opponent READY first.
void Calls()
{
    using Kind = runtime::TournamentCall::Kind;
    auto call = runtime::TournamentCallOf(Running("ready", -1, -1));
    CHECK(call.kind == Kind::SetReady && call.opponent == "Bowser" && call.tournament == "Recharged Cup");
    const auto first = call.key;
    call = runtime::TournamentCallOf(Running("ready", 1, 0));
    CHECK(call.kind == Kind::SetReady && call.key != first);
    // Playing: the player pressed READY (nothing), or the opponent did.
    auto playing = Running("playing", 1, 0);
    platform::ProfileMatch match;
    match.id = 12;
    match.you_ready = true;
    playing.match = match;
    CHECK(runtime::TournamentCallOf(playing).kind == Kind::None);
    playing.match->you_ready = false;
    playing.match->opponent_ready = true;
    call = runtime::TournamentCallOf(playing);
    CHECK(call.kind == Kind::OpponentReady && call.key == "opponent:12");
    // Waiting for the sets before it, out, or signed out: nothing.
    CHECK(runtime::TournamentCallOf(Running("pending", -1, -1)).kind == Kind::None);
    auto out = Running("ready", -1, -1);
    out.own->me->dropped = true;
    CHECK(runtime::TournamentCallOf(out).kind == Kind::None);
    out = Running("ready", -1, -1);
    out.signed_in = false;
    CHECK(runtime::TournamentCallOf(out).kind == Kind::None);
}
// The game a match goes on from, by the game's ids; none with a team unknown.
void Resume()
{
    platform::ProfileMatch match;
    CHECK(!runtime::TournamentResumeOf(match));
    platform::TournamentResume resume;
    resume.stadium = "the_palace";
    resume.seconds = 70;
    resume.you = {"bowser", {"birdo", "boo", "shy_guy"}, 1};
    resume.opponent = {"mario", {"toad", "boo", "koopa"}, 2};
    match.tournament.resume = resume;
    const auto made = runtime::TournamentResumeOf(match);
    CHECK(made && made->own_captain == 1 && made->opponent_captain == 0);
    CHECK(made && made->own_sidekicks[0] == 3 && made->own_sidekicks[1] == 4 && made->own_sidekicks[2] == 7);
    CHECK(made && made->opponent_sidekicks[2] == 1 && made->own_score == 1 && made->opponent_score == 2
          && made->seconds == 70.0f);
    match.tournament.resume->opponent.sidekicks.pop_back();
    CHECK(!runtime::TournamentResumeOf(match));
    match.tournament.resume = resume;
    match.tournament.resume->you.captain = "unknown";
    CHECK(!runtime::TournamentResumeOf(match));
}

// A set's games in the bracket: its card lists them, each with what the
// game's results screen shows (the captains, the goals, home first).
void Games()
{
    Snapshot feed = Running("finished", 1, 1);
    platform::TournamentPlayedGame won, broke;
    won.home_captain = "bowser_jr";
    won.away_captain = "donkey_kong";
    won.home_score = 10;
    won.away_score = 1;
    broke.home_captain = "donkey_kong";
    broke.away_captain = "bowser_jr";
    broke.home_score = 2;
    broke.interrupted = true;
    feed.page->sets[0].played = {won, broke};
    const auto model = runtime::ServerTournament(feed);
    CHECK(model.bracket.size() == 1 && model.bracket[0].games.size() == 2);
    if (model.bracket.size() != 1 || model.bracket[0].games.size() != 2) return;
    const auto& first = model.bracket[0].games[0];
    CHECK(first.text == "Game 1: Bowser Jr. 10 - 1 Donkey Kong");
    CHECK(first.captains[0] == 9 && first.captains[1] == 3 && first.goals[0] == 10 && first.goals[1] == 1);
    CHECK(model.bracket[0].games[1].text == "Game 2: Donkey Kong 2 - 0 Bowser Jr. (broke off)");
    // A captain the game does not know: no line for that game.
    feed.page->sets[0].played[0].home_captain = "unknown";
    CHECK(runtime::ServerTournament(feed).bracket[0].games.size() == 1);
}

// Single elimination with an ash bracket: the ash sets below the main ones,
// by their own round names; its final ends the tournament for nobody.
void Ash()
{
    Snapshot feed = Running("ready", 0, 0);
    auto& page = *feed.page;
    page.tournament.format = "single_ash";
    auto final_set = page.sets[0];
    final_set.side = "winners";
    final_set.round = 2;
    final_set.games = 5;
    auto ash_first = final_set, ash_final = final_set;
    ash_first.id = 31;
    ash_first.side = "ash";
    ash_first.round = 1;
    ash_first.state = "finished";
    ash_first.match = 0;
    ash_final.id = 32;
    ash_final.side = "ash";
    ash_final.round = 2;
    ash_final.match = 13;
    page.sets = {final_set, ash_first, ash_final};
    const auto model = runtime::ServerTournament(feed);
    CHECK(model.ash && model.event == "Single elimination + ash bracket  ·  Competitive");
    CHECK(model.bracket.size() == 3);
    if (model.bracket.size() != 3) return;
    CHECK(model.bracket[0].round == 2 && model.bracket[0].round_text == "Final");
    CHECK(model.bracket[1].round == -1 && model.bracket[1].round_text == "Ash Round 1");
    CHECK(model.bracket[2].round == -2 && model.bracket[2].round_text == "Ash Final");
    // One game from the set: the final's is the tournament's, the ash's not.
    page.sets[0].home_wins = page.sets[2].home_wins = 2;
    CHECK(runtime::TournamentEndingOf(page, 12).own);
    const auto ash_ending = runtime::TournamentEndingOf(page, 13);
    CHECK(!ash_ending.own && !ash_ending.opponent);
    feed.tournaments = {page.tournament};
    const auto list = runtime::ServerTournaments(feed);
    CHECK(list.featured && list.featured->lines[0] == "Single elimination + ash bracket  ·  Competitive");
}
} // namespace

int main()
{
    List();
    Calls();
    Resume();
    Games();
    Ash();
    if (failures)
    {
        std::fprintf(stderr, "%d server tournament checks failed\n", failures);
        return 1;
    }
    return 0;
}
