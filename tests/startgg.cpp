// start.gg's answers for the TOURNAMENT page: the request, the event and its
// sets read from saved answers (tests/fixtures/startgg, made up), the
// player's next set, the refusals (the rate limit, the token), and the
// sources (a file; start.gg through a test's transport), and the feed that
// asks only while the page is open.
#include "platform/json.h"
#include "platform/startgg.h"
#include "platform/startgg_feed.h"
#include "platform/startgg_source.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
    CHECK(first.winner == "501");
    CHECK(first.slots[0].id == "501");
    CHECK(first.slots[0].name == "Alice");
    CHECK(first.slots[0].user_ids.size() == 1 && first.slots[0].user_ids[0] == "11");
    CHECK(first.slots[0].user_discriminators.size() == 1 && first.slots[0].user_discriminators[0] == "a11ce0de");
    CHECK(StartggIsUser(first.slots[0], "11") && StartggIsUser(first.slots[0], "a11ce0de"));
    CHECK(!StartggIsUser(first.slots[0], "12") && !StartggIsUser(first.slots[0], ""));
    CHECK(StartggIsUser(first.slots[0], "entrant/501") && !StartggIsUser(first.slots[0], "entrant/50"));
    CHECK(!StartggIsUser(first.slots[0], "501") && !StartggIsUser(StartggEntrant{}, "entrant/"));
    CHECK(first.slots[1].user_discriminators.empty());
    CHECK(first.slots[0].score == 2);
    CHECK(first.slots[1].score == 0); // sent as 0.0
    CHECK(first.prereq[0].empty()); // a seed, not a set

    const StartggSet& semi = event.sets[1];
    CHECK(semi.state == StartggSetState::Created);
    CHECK(semi.winner.empty());
    CHECK(semi.prereq[0] == "1001" && semi.prereq[1] == "1003");
    CHECK(semi.prereq_placement[0] == 1 && semi.prereq_placement[1] == 1);
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
    CHECK(losers.prereq_placement[0] == 2 && losers.prereq_placement[1] == 2); // the losers'
    CHECK(first.prereq_placement[0] == 0); // a seed
}

void NextSet()
{
    const StartggEvent event = *ReadStartggAnswer(200, Fixture("event.json")).event;
    int slot = -9;
    // Alice: her semi-final (her first set is done).
    const StartggSet* set = StartggNextSet(event, "11", &slot);
    CHECK(set && set->id == "1002");
    CHECK(slot == 0);
    // Alice by her profile's discriminator.
    set = StartggNextSet(event, "a11ce0de", &slot);
    CHECK(set && set->id == "1002");
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
void Files()
{
    auto source = StartggFileSource(std::string(STARTGG_FIXTURES) + "/event.json");
    StartggAnswer answer = source->Fetch("ignored");
    CHECK(answer.event && answer.event->sets.size() == 4);
    answer = StartggFileSource(std::string(STARTGG_FIXTURES) + "/missing.json")->Fetch("x");
    CHECK(!answer.event);
    CHECK(answer.error.find("missing.json") != std::string::npos);
}

void Http()
{
    const std::string token = "secret-token-123";
    std::vector<HttpRequest> sent;
    // Two pages, as start.gg would answer them.
    auto twoPages = [&](const HttpRequest& request) {
        sent.push_back(request);
        const auto page = ParseJson(request.body)["variables"]["page"].Integer();
        return HttpResponse{200, Fixture(page == 1 ? "event_page1.json" : "event_page2.json")};
    };
    StartggAnswer answer = StartggHttpSource(token, kStartggApi, twoPages)->Fetch("tournament/t/event/e");
    CHECK(answer.event && answer.event->sets.size() == 2);
    CHECK(sent.size() == 2);
    if (!sent.empty())
    {
        const HttpRequest& request = sent[0];
        CHECK(request.url == kStartggApi);
        CHECK(request.method == "POST");
        CHECK(!request.allow_http);
        bool bearer = false, json = false;
        for (const auto& header : request.headers)
        {
            bearer = bearer || header == "Authorization: Bearer " + token;
            json = json || header == "Content-Type: application/json";
        }
        CHECK(bearer && json);
        CHECK(ParseJson(request.body)["variables"]["slug"].Text() == "tournament/t/event/e");
    }

    // A refusal on a later page: no event half read, and never the token.
    sent.clear();
    auto limited = [&](const HttpRequest& request) {
        sent.push_back(request);
        if (sent.size() == 1) return HttpResponse{200, Fixture("event_page1.json")};
        return HttpResponse{429, Fixture("rate_limit.json")};
    };
    answer = StartggHttpSource(token, kStartggApi, limited)->Fetch("e");
    CHECK(!answer.event);
    CHECK(answer.rate_limited);
    CHECK(answer.error.find(token) == std::string::npos);

    auto unreachable = [](const HttpRequest&) -> HttpResponse { throw HttpError("could not connect"); };
    answer = StartggHttpSource(token, kStartggApi, unreachable)->Fetch("e");
    CHECK(!answer.event);
    CHECK(answer.error.find("could not connect") != std::string::npos);
    CHECK(answer.error.find(token) == std::string::npos);

    // Without a token, or over plain http elsewhere than this computer:
    // nothing is sent.
    sent.clear();
    CHECK(!StartggHttpSource("", kStartggApi, twoPages)->Fetch("e").event);
    CHECK(!StartggHttpSource(token, "http://api.start.gg/gql/alpha", twoPages)->Fetch("e").event);
    CHECK(sent.empty());
    answer = StartggHttpSource(token, "http://127.0.0.1:8765/gql", twoPages)->Fetch("e");
    CHECK(answer.event.has_value());
    CHECK(!sent.empty() && sent[0].allow_http);
}
// A source that counts its fetches; rate limited while `limited`.
class CountingSource final : public StartggSource
{
public:
    std::atomic<int> fetches{0};
    std::atomic<bool> limited{false};
    StartggAnswer Fetch(std::string_view) override
    {
        ++fetches;
        if (limited) return ReadStartggAnswer(429, Fixture("rate_limit.json"));
        return ReadStartggAnswer(200, Fixture("event.json"));
    }
};

// Waits up to a second for `done`.
template <typename F> bool Within(F done)
{
    for (int n = 0; n < 200 && !done(); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return done();
}

void Feed()
{
    using namespace std::chrono_literals;
    auto owned = std::make_unique<CountingSource>();
    CountingSource& source = *owned;
    std::vector<std::string> logged;
    std::mutex logMutex;
    StartggFeed feed(std::move(owned), "e", [&](const std::string& line) {
        std::lock_guard lock(logMutex);
        logged.push_back(line);
    }, 150ms, 400ms);
    // Closed: nothing asked.
    std::this_thread::sleep_for(50ms);
    CHECK(source.fetches == 0);
    CHECK(!feed.Latest().loaded);
    // Opened: at once, then every interval.
    feed.SetOpen(true);
    CHECK(Within([&] { return feed.Latest().loaded; }));
    CHECK(feed.Latest().event && feed.Latest().error.empty());
    CHECK(Within([&] { return source.fetches >= 2; }));
    // Closed: no more.
    feed.SetOpen(false);
    std::this_thread::sleep_for(30ms); // a fetch that had started
    const int closedAt = source.fetches;
    std::this_thread::sleep_for(250ms);
    CHECK(source.fetches == closedAt);
    // Rate limited: the event stays, the error shows, and it waits longer.
    source.limited = true;
    feed.SetOpen(true);
    CHECK(Within([&] { return feed.Latest().rate_limited; }));
    CHECK(feed.Latest().event.has_value());
    const int limitedAt = source.fetches;
    std::this_thread::sleep_for(250ms); // past the interval, not the wait
    CHECK(source.fetches == limitedAt);
    source.limited = false;
    CHECK(Within([&] { return !feed.Latest().rate_limited && feed.Latest().error.empty(); }));
    feed.SetOpen(false);
    std::lock_guard lock(logMutex);
    CHECK(logged.size() == 2);
    CHECK(!logged.empty() && logged[0].find("rate limit") != std::string::npos);
}
} // namespace

int main()
{
    Request();
    Event();
    NextSet();
    Pages();
    Refusals();
    Files();
    Http();
    Feed();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::puts("startgg: all passed");
    return failures ? 1 : 0;
}
