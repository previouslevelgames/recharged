#include "runtime/startgg_tournament.h"

#include "bootstrap/texts.h"

#include <cctype>
#include <string>

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
    return initials;
}

const char* StateText(platform::StartggSetState state)
{
    switch (state)
    {
    case platform::StartggSetState::Active: return T("menus.tournament.state_active");
    case platform::StartggSetState::Called: return T("menus.tournament.state_called");
    default: return T("menus.tournament.state_waiting");
    }
}

bool InEvent(const platform::StartggEvent& event, std::string_view user)
{
    for (const auto& set : event.sets)
        for (const auto& slot : set.slots)
            if (platform::StartggIsUser(slot, user)) return true;
    return false;
}
} // namespace

ui::TournamentModel StartggTournament(const platform::StartggFeed::Snapshot& feed, std::string_view user)
{
    ui::TournamentModel model;
    model.games = 0; // start.gg's sets do not say their best of here
    if (!feed.event)
    {
        model.name = T("menus.tournament.title");
        model.note = !feed.loaded ? T("menus.tournament.loading")
                                  : T("menus.tournament.failed", {{"error", feed.error}});
        return model;
    }
    const platform::StartggEvent& event = *feed.event;
    model.name = event.tournament;
    model.event = event.name;
    if (!feed.error.empty()) model.note = T("menus.tournament.stale", {{"error", feed.error}});
    int mine = -1;
    const platform::StartggSet* set = platform::StartggNextSet(event, user, &mine);
    if (!set)
    {
        if (model.note.empty())
            model.note = InEvent(event, user) ? T("menus.tournament.no_set") : T("menus.tournament.not_in");
        return model;
    }
    const platform::StartggEntrant& opponent = set->slots[1 - mine];
    model.has_set = true;
    model.opponent_known = !opponent.id.empty();
    model.opponent = model.opponent_known ? opponent.name : T("menus.tournament.opponent_waiting");
    model.opponent_initials = model.opponent_known ? Initials(opponent.name) : "?";
    model.round = set->round_text + "  ·  " + StateText(set->state);
    const auto& own = set->slots[mine].score;
    if (own && opponent.score) model.score = std::to_string(*own) + " - " + std::to_string(*opponent.score);
    return model;
}
} // namespace mscharged::runtime
