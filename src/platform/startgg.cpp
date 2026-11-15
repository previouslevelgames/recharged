#include "platform/startgg.h"

#include "platform/json.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>

namespace mscharged::platform
{
const char* const kStartggEventQuery = R"(query RechargedEvent($slug: String!, $page: Int!, $perPage: Int!) {
  event(slug: $slug) {
    id
    name
    tournament { name }
    sets(page: $page, perPage: $perPage, sortType: STANDARD) {
      pageInfo { totalPages }
      nodes {
        id
        state
        round
        fullRoundText
        winnerId
        phaseGroup { id }
        slots {
          prereqId
          entrant { id name participants { user { id discriminator } } }
          standing { stats { score { value } } }
        }
      }
    }
  }
})";

std::string StartggEventRequest(std::string_view slug, int page)
{
    std::string body = "{\"query\":";
    AppendJsonString(body, kStartggEventQuery);
    body += ",\"variables\":{\"slug\":";
    AppendJsonString(body, slug);
    body += ",\"page\":" + std::to_string(page) + ",\"perPage\":" + std::to_string(kStartggSetsPerPage) + "}}";
    return body;
}

namespace
{
// An id as text: start.gg sends numbers, and strings for preview sets.
std::string Id(const JsonValue& value) { return value.IsNull() ? std::string{} : value.Text(); }

int Int(const JsonValue& value, int fallback = 0)
{
    if (const auto whole = value.Integer()) return int(*whole);
    // A float ("2.0"): its whole part.
    if (value.type() == JsonValue::Type::Number)
    {
        const double number = std::strtod(value.Text().c_str(), nullptr);
        if (std::isfinite(number)) return int(number);
    }
    return fallback;
}

StartggEntrant ReadEntrant(const JsonValue& slot)
{
    StartggEntrant entrant;
    const JsonValue& value = slot["entrant"];
    entrant.id = Id(value["id"]);
    entrant.name = value["name"].Text();
    for (const JsonValue& participant : value["participants"].Items())
    {
        if (const std::string user = Id(participant["user"]["id"]); !user.empty()) entrant.user_ids.push_back(user);
        if (const std::string& tag = participant["user"]["discriminator"].Text(); !tag.empty())
            entrant.user_discriminators.push_back(tag);
    }
    const JsonValue& score = slot["standing"]["stats"]["score"]["value"];
    if (score.type() == JsonValue::Type::Number) entrant.score = Int(score);
    return entrant;
}

StartggSet ReadSet(const JsonValue& node)
{
    StartggSet set;
    set.id = Id(node["id"]);
    const int state = Int(node["state"]);
    set.state = state >= 1 && state <= 7 ? StartggSetState(state) : StartggSetState::Unknown;
    set.round = Int(node["round"]);
    set.round_text = node["fullRoundText"].Text();
    set.phase_group = Id(node["phaseGroup"]["id"]);
    set.winner = Id(node["winnerId"]);
    const auto& slots = node["slots"].Items();
    for (std::size_t n = 0; n < 2 && n < slots.size(); ++n)
    {
        set.slots[n] = ReadEntrant(slots[n]);
        set.prereq[n] = Id(slots[n]["prereqId"]);
    }
    return set;
}

// The first error's message, or empty.
std::string FirstError(const JsonValue& root)
{
    for (const JsonValue& error : root["errors"].Items())
        if (!error["message"].Text().empty()) return error["message"].Text();
    // start.gg's own refusals (the token, the rate limit): {success, message}.
    if (root["success"].type() == JsonValue::Type::Boolean && !root["success"].Bool())
        return root["message"].Text().empty() ? "refused" : root["message"].Text();
    return {};
}
} // namespace

StartggAnswer ReadStartggAnswer(long status, std::string_view body)
{
    StartggAnswer answer;
    JsonValue root;
    try
    {
        root = ParseJson(body);
    }
    catch (const std::exception&)
    {
        answer.error = "start.gg answered " + std::to_string(status) + " without JSON";
        answer.rate_limited = status == 429;
        return answer;
    }
    const std::string error = FirstError(root);
    answer.rate_limited = status == 429 || error.find("Rate limit exceeded") != std::string::npos;
    if (answer.rate_limited)
    {
        answer.error = "start.gg: rate limit exceeded";
        return answer;
    }
    if (status == 401 || status == 403)
    {
        answer.error = "start.gg refused the token";
        if (!error.empty()) answer.error += " (" + error + ")";
        return answer;
    }
    if (!error.empty())
    {
        answer.error = "start.gg: " + error;
        return answer;
    }
    if (status != 200)
    {
        answer.error = "start.gg answered " + std::to_string(status);
        return answer;
    }
    const JsonValue& value = root["data"]["event"];
    if (!value.IsObject())
    {
        answer.error = "start.gg has no such event";
        return answer;
    }
    StartggEvent event;
    event.id = Id(value["id"]);
    event.name = value["name"].Text();
    event.tournament = value["tournament"]["name"].Text();
    const JsonValue& sets = value["sets"];
    event.total_pages = std::max(1, Int(sets["pageInfo"]["totalPages"], 1));
    for (const JsonValue& node : sets["nodes"].Items()) event.sets.push_back(ReadSet(node));
    answer.event = std::move(event);
    return answer;
}

void AddStartggPage(StartggEvent& event, StartggEvent page)
{
    for (auto& set : page.sets) event.sets.push_back(std::move(set));
}

bool StartggIsUser(const StartggEntrant& entrant, std::string_view user)
{
    if (user.empty()) return false;
    for (const auto& id : entrant.user_ids)
        if (id == user) return true;
    for (const auto& tag : entrant.user_discriminators)
        if (tag == user) return true;
    return false;
}

bool StartggSetOpen(StartggSetState state)
{
    switch (state)
    {
    case StartggSetState::Created:
    case StartggSetState::Active:
    case StartggSetState::Ready:
    case StartggSetState::Called:
    case StartggSetState::Queued: return true;
    default: return false;
    }
}

const StartggSet* StartggNextSet(const StartggEvent& event, std::string_view user_id, int* slot)
{
    if (user_id.empty()) return nullptr;
    const StartggSet* best = nullptr;
    int bestRank = 0, bestSlot = -1;
    for (const StartggSet& set : event.sets)
    {
        if (!StartggSetOpen(set.state)) continue;
        int mine = -1;
        for (int n = 0; n < 2 && mine < 0; ++n)
            if (StartggIsUser(set.slots[n], user_id)) mine = n;
        if (mine < 0) continue;
        // Being played or called first, then with its opponent known.
        const bool going = set.state == StartggSetState::Active || set.state == StartggSetState::Called;
        const bool opponent = !set.slots[1 - mine].id.empty();
        const int rank = going ? 3 : opponent ? 2 : 1;
        if (rank > bestRank)
        {
            best = &set;
            bestRank = rank;
            bestSlot = mine;
        }
    }
    if (slot) *slot = bestSlot;
    return best;
}
} // namespace mscharged::platform
