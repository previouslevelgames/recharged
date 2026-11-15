// start.gg's answers for the TOURNAMENT page: the request, the event and its
// sets read from saved answers (tests/fixtures/startgg, made up), the
// player's next set, and the refusals (the rate limit, the token).
#include "platform/json.h"
#include "platform/startgg.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace mscharged::platform;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

std::string Fixture(const char* name)
{
    std::ifstream file(std::string(STARTGG_FIXTURES) + "/" + name, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void Request()
{
    const JsonValue body = ParseJson(StartggEventRequest("tournament/test-cup/event/singles", 3));
    CHECK(body["query"].Text() == kStartggEventQuery);
    CHECK(body["query"].Text().find("event(slug: $slug)") != std::string::npos);
    CHECK(body["variables"]["slug"].Text() == "tournament/test-cup/event/singles");
    CHECK(body["variables"]["page"].Integer() == 3);
    CHECK(body["variables"]["perPage"].Integer() == kStartggSetsPerPage);
    // A slug with a quote stays one string.
    CHECK(ParseJson(StartggEventRequest("a\"b", 1))["variables"]["slug"].Text() == "a\"b");
}

void Event()
{
    const StartggAnswer answer = ReadStartggAnswer(200, Fixture("event.json"));
    CHECK(answer.error.empty());
    CHECK(!answer.rate_limited);
    CHECK(answer.event.has_value());
    if (!answer.event) return;
    const StartggEvent& event = *answer.event;
    CHECK(event.id == "900");
    CHECK(event.name == "Mario Strikers Charged Singles");
    CHECK(event.tournament == "Recharged Test Cup");
    CHECK(event.total_pages == 1);
    CHECK(event.sets.size() == 4);
    if (event.sets.size() != 4) return;

    const StartggSet& first = event.sets[0];
    CHECK(first.id == "1001");
    CHECK(first.state == StartggSetState::Completed);
    CHECK(first.round == 1);
    CHECK(first.round_text == "Winners Round 1");
    CHECK(first.phase_group == "70");
    CHECK(first.slots[0].id == "501");
    CHECK(first.slots[0].name == "Alice");
    CHECK(first.slots[0].user_ids.size() == 1 && first.slots[0].user_ids[0] == "11");
    CHECK(first.slots[0].score == 2);
    CHECK(first.slots[1].score == 0); // sent as 0.0
    CHECK(first.prereq[0].empty());

    const StartggSet& semi = event.sets[1];
    CHECK(semi.state == StartggSetState::Created);
    CHECK(semi.prereq[0] == "1001" && semi.prereq[1] == "1003");
    CHECK(!semi.slots[0].score.has_value()); // no standing yet

    // A participant without a start.gg account, a score not reported.
    const StartggSet& active = event.sets[2];
    CHECK(active.state == StartggSetState::Active);
    CHECK(active.slots[1].name == "Dave");
    CHECK(active.slots[1].user_ids.empty());
    CHECK(!active.slots[1].score.has_value());

    // A preview set (the bracket not started): its id is text; a slot open.
    const StartggSet& losers = event.sets[3];
    CHECK(losers.id == "preview_70_4");
    CHECK(losers.round == -1);
    CHECK(losers.slots[1].id.empty());
    CHECK(losers.slots[1].name.empty());
}

void NextSet()
{
    const StartggEvent event = *ReadStartggAnswer(200, Fixture("event.json")).event;
    int slot = -9;
    // Alice: her semi-final (her first set is done).
    const StartggSet* set = StartggNextSet(event, "11", &slot);
    CHECK(set && set->id == "1002");
    CHECK(slot == 0);
    // Carol: the set being played comes before the next one.
    set = StartggNextSet(event, "13", &slot);
    CHECK(set && set->id == "1003");
    CHECK(slot == 0);
    // Bob: a losers' set whose opponent is not known yet.
    set = StartggNextSet(event, "12", &slot);
    CHECK(set && set->id == "preview_70_4");
    CHECK(slot == 0);
    // Not in the event, or no user: none.
    CHECK(StartggNextSet(event, "99", &slot) == nullptr);
    CHECK(slot == -1);
    CHECK(StartggNextSet(event, "") == nullptr);
    // All played.
    const StartggEvent done = *ReadStartggAnswer(200, Fixture("no_open.json")).event;
    CHECK(StartggNextSet(done, "11") == nullptr);
}

void Pages()
{
    StartggEvent event = *ReadStartggAnswer(200, Fixture("event_page1.json")).event;
    CHECK(event.total_pages == 2);
    AddStartggPage(event, *ReadStartggAnswer(200, Fixture("event_page2.json")).event);
    CHECK(event.sets.size() == 2);
    CHECK(event.tournament == "Two Pages");
    const StartggSet* set = StartggNextSet(event, "11");
    CHECK(set && set->id == "3002" && set->state == StartggSetState::Called);
}

void Refusals()
{
    StartggAnswer answer = ReadStartggAnswer(429, Fixture("rate_limit.json"));
    CHECK(answer.rate_limited);
    CHECK(!answer.event);
    // The same message with another status.
    CHECK(ReadStartggAnswer(200, Fixture("rate_limit.json")).rate_limited);
    CHECK(ReadStartggAnswer(429, "Too Many Requests").rate_limited);

    answer = ReadStartggAnswer(401, R"({"success":false,"message":"Invalid authentication token"})");
    CHECK(!answer.rate_limited);
    CHECK(answer.error.find("token") != std::string::npos);

    answer = ReadStartggAnswer(200, Fixture("no_event.json"));
    CHECK(!answer.event);
    CHECK(answer.error == "start.gg has no such event");

    answer = ReadStartggAnswer(200, R"({"errors":[{"message":"Variable \"$slug\" is invalid"}]})");
    CHECK(!answer.event);
    CHECK(answer.error.find("$slug") != std::string::npos);

    answer = ReadStartggAnswer(502, "<html>Bad Gateway</html>");
    CHECK(!answer.event && !answer.rate_limited);
    CHECK(answer.error.find("502") != std::string::npos);
}
} // namespace

int main()
{
    Request();
    Event();
    NextSet();
    Pages();
    Refusals();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("startgg: all passed");
    return failures ? 1 : 0;
}
