// The WFC server's match reports: the series rule the game module applies to
// each result, the report bodies the host makes of the module's series, and
// the sender (order, retries, dropping, stopping) against a fake server.
#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/online_reports.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mscharged::platform;

namespace
{
int sChecks = 0;

void Require(bool condition, const std::string& message)
{
    ++sChecks;
    if (!condition) throw std::runtime_error(message);
}

ChargedOnlineGame Game(int home, int away, int ending = 0, int stadium = 10, float seconds = 180.0f)
{
    ChargedOnlineGame game{};
    game.scores[0] = home;
    game.scores[1] = away;
    game.captains[0] = 0; // Mario
    game.captains[1] = 1; // Bowser
    game.stadium = stadium;
    game.ending = ending;
    game.seconds = seconds;
    return game;
}

ChargedOnlineMatchSnapshot Series(unsigned number, int bestOf)
{
    ChargedOnlineMatchSnapshot s{};
    s.series = number;
    s.profile_id = 1000000042;
    s.ranked = 1;
    s.best_of = bestOf;
    s.side = 0;
    std::strcpy(s.mii[0], "Anosch");
    std::strcpy(s.mii[1], "Sam");
    s.captains[0] = 0;
    s.captains[1] = 1;
    const int sidekicks[2][3] = {{0, 1, 4}, {3, 4, 5}};
    std::memcpy(s.sidekicks, sidekicks, sizeof sidekicks);
    return s;
}

void Add(ChargedOnlineMatchSnapshot& s, const ChargedOnlineGame& game, int result = 0)
{
    ChargedOnlineSeriesResult(s, result, false, &game);
}

void TestSeriesRule()
{
    // Best of 3: decided by the second win, whoever has it.
    auto s = Series(1, 3);
    Add(s, Game(3, 1));
    Require(s.games == 1 && s.ended == 0, "best of 3, one win: under way");
    Add(s, Game(0, 2));
    Require(s.games == 2 && s.ended == 0, "best of 3, 1-1: under way");
    Add(s, Game(2, 1));
    Require(s.games == 3 && s.ended == 1, "best of 3, 2-1: decided");
    Add(s, Game(5, 0));
    Require(s.games == 3, "a decided series takes no more games");

    s = Series(1, 3);
    Add(s, Game(1, 4));
    Add(s, Game(0, 1));
    Require(s.games == 2 && s.ended == 1, "best of 3, 0-2: decided");

    s = Series(1, 1);
    Add(s, Game(2, 2));
    Require(s.games == 1 && s.ended == 0, "a draw decides nothing");
    Add(s, Game(1, 0));
    Require(s.games == 2 && s.ended == 1, "best of 1 decided by its win");

    // A result for a game already settled: a lost connection on the screens
    // after it stops the series without a second game; a normal one changes
    // nothing.
    s = Series(2, 5);
    Add(s, Game(3, 0));
    ChargedOnlineSeriesResult(s, 0, true, nullptr);
    Require(s.games == 1 && s.ended == 0, "a settled game's repeat adds nothing");
    const ChargedOnlineGame again = Game(3, 0, 4);
    ChargedOnlineSeriesResult(s, 4, true, &again);
    Require(s.games == 1 && s.ended == 2, "a lost connection after a settled game stops the series");

    // A lost connection before a game began stops the series without a game.
    s = Series(3, 3);
    ChargedOnlineSeriesResult(s, 4, false, nullptr);
    Require(s.games == 0 && s.ended == 2, "a connection lost before a game stops the series");

    // One lost during a game ends both, with the score this side saw.
    s = Series(4, 3);
    Add(s, Game(1, 0));
    Add(s, Game(2, 1, 4), 4);
    Require(s.games == 2 && s.ended == 2 && s.game[1].ending == 4 && s.game[1].scores[0] == 2,
            "a connection lost in a game ends it and the series");
    s = Series(5, 3);
    Add(s, Game(0, 0, 2), 2);
    Require(s.games == 1 && s.ended == 2, "a sync error ends the series");

    // No series (none begun) takes nothing; the games fit their array.
    s = ChargedOnlineMatchSnapshot{};
    Add(s, Game(1, 0));
    Require(s.games == 0 && s.ended == 0, "no series, no game");
    s = Series(6, 0);
    for (int n = 0; n < 12; ++n) Add(s, Game(n % 2, 1 - n % 2));
    Require(s.games == 9 && s.ended == 0, "games kept up to the array's size");
}

std::vector<JsonValue> Parse(const std::vector<std::string>& bodies)
{
    std::vector<JsonValue> events;
    for (const auto& body : bodies) events.push_back(ParseJson(body));
    return events;
}

void TestEvents()
{
    auto s = Series(1, 3);
    s.profile_id = 0;
    Require(OnlineMatchEvents(nullptr, s, "R4QE", 7).empty(), "no profile, no reports");
    s = ChargedOnlineMatchSnapshot{};
    Require(OnlineMatchEvents(nullptr, s, "R4QE", 7).empty(), "no series, no reports");

    // A series seen as it begins, then game by game, then its end.
    s = Series(1, 3);
    auto events = Parse(OnlineMatchEvents(nullptr, s, "R4QE", 1761500000));
    Require(events.size() == 1, "a new series: its start");
    const JsonValue& start = events[0];
    Require(start["event"].Text() == "start" && start["profile_id"].Integer() == 1000000042
                && start["release"].Text() == "R4QE" && start["series"].Integer() == 1761500000
                && start["mode"].Text() == "ranked" && start["best_of"].Integer() == 3
                && start["side"].Text() == "home",
            "the start's fields");
    Require(start["mii"].Text() == "Anosch" && start["opponent_mii"].Text() == "Sam", "the start's Miis");
    Require(start["captains"]["home"].Text() == "mario" && start["captains"]["away"].Text() == "bowser",
            "the start's captains");
    const JsonValue& home = start["sidekicks"]["home"];
    const JsonValue& away = start["sidekicks"]["away"];
    Require(home.size() == 3 && home.Items()[0].Text() == "toad" && home.Items()[1].Text() == "koopa"
                && home.Items()[2].Text() == "boo",
            "home sidekicks");
    Require(away.size() == 3 && away.Items()[0].Text() == "birdo" && away.Items()[2].Text() == "dry_bones",
            "away sidekicks");
    Require(start["game"].IsNull() && start["reason"].IsNull(), "a start has no game and no reason");

    auto last = s;
    Add(s, Game(3, 1, 0, 10, 179.6f));
    events = Parse(OnlineMatchEvents(&last, s, "R4QE", 1761500000));
    Require(events.size() == 1 && events[0]["event"].Text() == "game", "one game ended: its report");
    const JsonValue& game = events[0]["game"];
    Require(game["number"].Integer() == 1 && game["score"]["home"].Integer() == 3
                && game["score"]["away"].Integer() == 1 && game["stadium"].Text() == "the_palace"
                && game["ending"].Text() == "normal" && game["seconds"].Integer() == 180
                && game["captains"]["home"].Text() == "mario",
            "the game's fields");
    Require(events[0]["reason"].IsNull() && events[0]["series"].Integer() == 1761500000,
            "a game report: same series, no reason");
    Require(OnlineMatchEvents(&s, s, "R4QE", 1761500000).empty(), "nothing new, no reports");

    // Two games and the end seen at once.
    last = s;
    Add(s, Game(0, 2, 0, 15));
    Add(s, Game(2, 1, 0, -1));
    events = Parse(OnlineMatchEvents(&last, s, "R4QE", 1761500000));
    Require(events.size() == 3 && events[0]["game"]["number"].Integer() == 2
                && events[0]["game"]["stadium"].Text() == "sand_tomb" && events[1]["game"]["number"].Integer() == 3
                && events[1]["game"]["stadium"].Text() == "unknown" && events[2]["event"].Text() == "end"
                && events[2]["reason"].Text() == "decided" && events[2]["game"].IsNull(),
            "two games and a decided end");
    Require(OnlineMatchEvents(&s, s, "R4QE", 1761500000).empty(), "an end is reported once");

    // A new series number begins another: start, games, end at once.
    last = s;
    s = Series(2, 3);
    s.ranked = 0;
    s.side = 1;
    Add(s, Game(1, 2, 4), 4);
    events = Parse(OnlineMatchEvents(&last, s, "R4QP", 1761500900));
    Require(events.size() == 3 && events[0]["event"].Text() == "start" && events[0]["mode"].Text() == "friends"
                && events[0]["release"].Text() == "R4QP" && events[0]["series"].Integer() == 1761500900,
            "a new series begins with its start");
    Require(events[0]["side"].Text() == "away" && events[0]["mii"].Text() == "Sam"
                && events[0]["opponent_mii"].Text() == "Anosch",
            "the away side reports its own Mii");
    Require(events[1]["game"]["ending"].Text() == "disconnect" && events[2]["reason"].Text() == "abandoned",
            "a lost connection: disconnect, abandoned");

    // Names: UTF-8 and quotes as JSON, a name filling its buffer, unknown
    // captains and sidekicks.
    s = Series(3, 1);
    std::strcpy(s.mii[0], "Zo\xc3\xab \"Z\"");
    std::memset(s.mii[1], 'x', sizeof s.mii[1]);
    s.captains[0] = -1;
    s.captains[1] = 12;
    s.sidekicks[0][1] = -1;
    s.sidekicks[1][0] = 8;
    s.sidekicks[1][1] = -1;
    s.sidekicks[1][2] = -1;
    events = Parse(OnlineMatchEvents(nullptr, s, "R4QE", 5));
    Require(events[0]["mii"].Text() == "Zo\xc3\xab \"Z\"", "a UTF-8 name with quotes");
    Require(events[0]["opponent_mii"].Text() == std::string(sizeof s.mii[1], 'x'), "a name filling its buffer");
    Require(events[0]["captains"]["home"].Text() == "unknown" && events[0]["captains"]["away"].Text() == "unknown",
            "unknown captains");
    Require(events[0]["sidekicks"]["home"].size() == 2 && events[0]["sidekicks"]["away"].size() == 0,
            "unknown sidekicks left out");

    // Out of range values stay readable: an odd ending, a broken clock.
    s = Series(4, 1);
    last = s;
    Add(s, Game(1, 0, 7, 16, -3.0f));
    events = Parse(OnlineMatchEvents(&last, s, "R4QE", 6));
    Require(events[0]["game"]["ending"].Text() == "normal" && events[0]["game"]["stadium"].Text() == "unknown"
                && events[0]["game"]["seconds"].Integer() == 0,
            "odd values reported as normal, unknown, 0");
}

// The fake server: records bodies, answers from a script of results.
struct FakeServer
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> bodies;
    std::vector<bool> answers; // per call; true after the script ends
    std::size_t calls = 0;

    OnlineReports::Send Send()
    {
        return [this](const std::string& body, const std::shared_ptr<HttpCancel>&) {
            std::lock_guard lock(mutex);
            const bool answer = calls < answers.size() ? answers[calls] : true;
            ++calls;
            if (answer) bodies.push_back(body);
            changed.notify_all();
            return answer;
        };
    }

    bool WaitFor(std::size_t count)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [&] { return bodies.size() >= count; });
    }

    std::size_t Calls()
    {
        std::lock_guard lock(mutex);
        return calls;
    }
};

void TestSender()
{
    using namespace std::chrono_literals;
    // In order; a failed report tried again, the series numbered from the
    // clock and kept for its reports.
    {
        FakeServer server;
        server.answers = {true, false, false, true};
        std::vector<std::string> log;
        std::mutex logMutex;
        {
            OnlineReports reports("R4QE", server.Send(), [&](const std::string& line) {
                std::lock_guard lock(logMutex);
                log.push_back(line);
            }, 10ms);
            auto s = Series(1, 3);
            reports.Observe(s);
            Add(s, Game(1, 0));
            reports.Observe(s);
            reports.Observe(s);
            Require(server.WaitFor(2), "two reports delivered");
            Require(server.Calls() == 4, "the second report took three tries");
            const auto start = ParseJson(server.bodies[0]);
            const auto game = ParseJson(server.bodies[1]);
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            Require(start["event"].Text() == "start" && game["event"].Text() == "game", "reports in order");
            Require(start["series"].Integer() == game["series"].Integer()
                        && *start["series"].Integer() > now - 60 && *start["series"].Integer() <= now,
                    "the series is numbered from the clock, the same in its reports");

            // A new module series takes another number, even within a second.
            auto next = Series(2, 1);
            reports.Observe(next);
            Require(server.WaitFor(3), "the next series' start delivered");
            Require(*ParseJson(server.bodies[2])["series"].Integer() > *start["series"].Integer(),
                    "the next series has a greater number");
        }
        std::lock_guard lock(logMutex);
        Require(log.empty(), "nothing to log when reports get through");
    }

    // A report that never gets through is dropped after four tries; the next goes on.
    {
        FakeServer server;
        server.answers = {false, false, false, false, true};
        std::vector<std::string> log;
        std::mutex logMutex;
        {
            OnlineReports reports("R4QE", server.Send(), [&](const std::string& line) {
                std::lock_guard lock(logMutex);
                log.push_back(line);
            }, 5ms);
            auto s = Series(1, 1);
            reports.Observe(s);
            Add(s, Game(2, 0));
            reports.Observe(s);
            Require(server.WaitFor(2), "the reports after a dropped one delivered");
            Require(server.Calls() == 6 && ParseJson(server.bodies[0])["event"].Text() == "game"
                        && ParseJson(server.bodies[1])["event"].Text() == "end",
                    "the start dropped after four tries, then the game and the end");
        }
        std::lock_guard lock(logMutex);
        Require(log.size() == 1 && log[0].find("dropped") != std::string::npos, "a dropped report is logged");
    }

    // Ending the reports stops a request under way at once.
    {
        std::atomic<bool> sending{false}, cancelled{false};
        auto started = std::chrono::steady_clock::now();
        {
            OnlineReports reports("R4QE", [&](const std::string&, const std::shared_ptr<HttpCancel>& cancel) {
                sending = true;
                const auto until = std::chrono::steady_clock::now() + 30s;
                while (!cancel->Cancelled() && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(1ms);
                cancelled = cancel->Cancelled();
                return false;
            }, nullptr, 10s);
            reports.Observe(Series(1, 1));
            const auto until = std::chrono::steady_clock::now() + 10s;
            while (!sending && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
            Require(sending, "the request began");
            started = std::chrono::steady_clock::now();
        }
        Require(cancelled, "the request was cancelled");
        Require(std::chrono::steady_clock::now() - started < 5s, "the reports ended at once");
    }
}
} // namespace

int main()
{
    try
    {
        TestSeriesRule();
        TestEvents();
        TestSender();
    }
    catch (const std::exception& error)
    {
        std::cerr << "online_reports: FAIL after " << sChecks << " checks: " << error.what() << '\n';
        return 1;
    }
    std::cout << "online_reports: " << sChecks << " checks passed\n";
    return 0;
}
