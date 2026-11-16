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
          && model.featured->lines[1] == "Sets best of 3, finals best of 5" && model.featured->lines[2] == "3 of 8 in");
    CHECK(model.others.size() == 1 && model.others[0].key == "9" && model.others[0].kicker == "OPEN FOR ENTRIES"
          && model.others[0].value == "3 of 8 in");
    CHECK(model.past.size() == 1 && model.past[0].key == "6" && model.past[0].value == "2nd"
          && model.past[0].lines.back() == "You finished 2nd");
    // Nobody's: the newest open for entries.
    feed.tournaments[1].joined = false;
    CHECK(runtime::ServerTournaments(feed).featured->key == "9");
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
    feed.page = page;
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
    out.page->me->dropped = true;
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
} // namespace

int main()
{
    List();
    Calls();
    Resume();
    if (failures)
    {
        std::fprintf(stderr, "%d server tournament checks failed\n", failures);
        return 1;
    }
    return 0;
}
