#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// start.gg (https://developer.start.gg): one event of a tournament and its
// sets, as its GraphQL API (https://api.start.gg/gql/alpha) answers, for the
// TOURNAMENT page over the trophy room. Reading only: the results go back
// to start.gg another way (the organizer, later the Recharged server).
namespace mscharged::platform
{
// A set's state, as start.gg's ActivityState numbers it.
enum class StartggSetState
{
    Unknown = 0,
    Created = 1, // not started
    Active = 2,  // being played
    Completed = 3,
    Ready = 4,
    Invalid = 5,
    Called = 6, // the players are called to play it
    Queued = 7,
};

struct StartggEntrant
{
    std::string id;   // the entrant's (empty: the slot is still open)
    std::string name; // as the event shows it (a team tag included)
    // The start.gg users of its participants (one in singles; empty for a
    // participant without an account).
    std::vector<std::string> user_ids;
    std::optional<int> score; // games won in the set, once reported
};

struct StartggSet
{
    std::string id; // a number, or "preview_..." for a bracket not started
    StartggSetState state = StartggSetState::Unknown;
    int round = 0;          // > 0 in winners' (or a single bracket), < 0 in losers'
    std::string round_text; // "Winners Round 1"
    std::string phase_group; // its pool or bracket
    StartggEntrant slots[2];
    // Where each slot's entrant comes from: the set whose winner or loser
    // takes it (empty for a seed).
    std::string prereq[2];
};

struct StartggEvent
{
    std::string id, name;
    std::string tournament;
    std::vector<StartggSet> sets;
    int total_pages = 1; // of the sets, as the answer says
};

// The query: an event by its slug ("tournament/<t>/event/<e>"), one page
// of its sets. A set costs about 16 of the 1000 objects an answer may hold.
extern const char* const kStartggEventQuery;
inline constexpr int kStartggSetsPerPage = 40;
// The request's body for page `page` (from 1) of the event `slug`.
std::string StartggEventRequest(std::string_view slug, int page);

// An answer read: the event, or why not. `rate_limited` when start.gg says
// the token asked too often (wait before asking again).
struct StartggAnswer
{
    std::optional<StartggEvent> event;
    std::string error;
    bool rate_limited = false;
};
// The answer's body with its HTTP status (200 for a file).
StartggAnswer ReadStartggAnswer(long status, std::string_view body);
// A page's sets added to `event` (its other fields stay the first page's).
void AddStartggPage(StartggEvent& event, StartggEvent page);

// Whether the set is still to be played (or being played).
bool StartggSetOpen(StartggSetState state);
// The player's next set in the event: of their open sets (`user_id`, their
// start.gg user's id), the one being played or called first, then one whose
// opponent is known, in the event's order. Its slot of the player in *slot.
const StartggSet* StartggNextSet(const StartggEvent& event, std::string_view user_id, int* slot = nullptr);
} // namespace mscharged::platform
