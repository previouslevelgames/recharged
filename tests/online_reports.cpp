// The WFC server's match reports: the series rule the game module applies to
// each result, the report bodies the host makes of the module's series and
// error codes, and the sender (paths, order, retries, dropping, stopping, the
// server's answers) against a fake server.
#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/online_reports.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace mscharged::platform;

namespace
{
int sChecks = 0;
const std::string kBuild = "1.1.0+g0123456789ab";
const std::string kReports = "/api/v1/wfc/reports";
const std::string kErrors = "/api/v1/wfc/errors";

void Require(bool condition, const std::string& message)
{
    ++sChecks;
    if (!condition) throw std::runtime_error(message);
}

bool Has(const JsonValue& object, std::string_view name)
{
    return std::find(object.Names().begin(), object.Names().end(), name) != object.Names().end();
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
    Require(s.games == 3 && s.ended == 1 && s.stopped_by == 0, "best of 3, 2-1: decided");
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
    Require(s.games == 1 && s.ended == 2 && s.stopped_by == 4,
            "a lost connection after a settled game stops the series");
    s = Series(2, 5);
    ChargedOnlineSeriesResult(s, 2, true, nullptr);
    Require(s.games == 0 && s.ended == 2 && s.stopped_by == 2, "so does a sync error");

    // A lost connection before a game began stops the series without a game.
    s = Series(3, 3);
    ChargedOnlineSeriesResult(s, 4, false, nullptr);
    Require(s.games == 0 && s.ended == 2 && s.stopped_by == 4, "a connection lost before a game stops the series");

    // One lost during a game ends both, with the score this side saw.
    s = Series(4, 3);
    Add(s, Game(1, 0));
    Add(s, Game(2, 1, 4), 4);
    Require(s.games == 2 && s.ended == 2 && s.stopped_by == 4 && s.game[1].ending == 4 && s.game[1].scores[0] == 2,
            "a connection lost in a game ends it and the series");
    s = Series(5, 3);
    Add(s, Game(0, 0, 2), 2);
    Require(s.games == 1 && s.ended == 2 && s.stopped_by == 2, "a sync error ends the series");
    // Even the game that would decide it: a game out of sync decides nothing.
    s = Series(5, 1);
    Add(s, Game(2, 0, 2), 2);
    Require(s.games == 1 && s.ended == 2 && s.stopped_by == 2, "a deciding score out of sync stops the series");

    // No series (none begun) takes nothing; the games fit their array.
    s = ChargedOnlineMatchSnapshot{};
    Add(s, Game(1, 0));
    Require(s.games == 0 && s.ended == 0, "no series, no game");
    s = Series(6, 0);
    for (int n = 0; n < 12; ++n) Add(s, Game(n % 2, 1 - n % 2));
    Require(s.games == 9 && s.ended == 0, "games kept up to the array's size");

    // The sync check a game out of sync failed (NetworkSyncState's frames,
    // -1 none): the first by frame; on one frame, the first the game checks.
    ChargedOnlineGame game = Game(1, 1, 2);
    ChargedOnlineSyncCheck(game, -1, 4812, -1);
    Require(game.sync_check == 2 && game.sync_frame == 4812, "a checksum mismatch");
    game = Game(1, 1, 2);
    ChargedOnlineSyncCheck(game, 900, 870, 905);
    Require(game.sync_check == 2 && game.sync_frame == 870, "the first mismatch by frame");
    game = Game(1, 1, 2);
    ChargedOnlineSyncCheck(game, 870, 870, 870);
    Require(game.sync_check == 1 && game.sync_frame == 870, "on one frame, the frames' check");
    game = Game(1, 1, 2);
    ChargedOnlineSyncCheck(game, -1, -1, 0);
    Require(game.sync_check == 3 && game.sync_frame == 0, "a random seed mismatch on frame 0");
    game = Game(1, 1, 2);
    ChargedOnlineSyncCheck(game, -1, -1, -1);
    Require(game.sync_check == 0, "no mismatch: unknown");
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
    Require(OnlineMatchEvents(nullptr, s, "R4QE", kBuild, 7).empty(), "no profile, no reports");
    s = ChargedOnlineMatchSnapshot{};
    Require(OnlineMatchEvents(nullptr, s, "R4QE", kBuild, 7).empty(), "no series, no reports");

    // A series seen as it begins, then game by game, then its end.
    s = Series(1, 3);
    auto events = Parse(OnlineMatchEvents(nullptr, s, "R4QE", kBuild, 1761500000));
    Require(events.size() == 1, "a new series: its start");
    const JsonValue& start = events[0];
    Require(start["event"].Text() == "start" && start["profile_id"].Integer() == 1000000042
                && start["release"].Text() == "R4QE" && start["build"].Text() == kBuild
                && start["series"].Integer() == 1761500000 && start["mode"].Text() == "ranked"
                && start["best_of"].Integer() == 3 && start["side"].Text() == "home",
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
    Require(start["game"].IsNull() && start["reason"].IsNull() && !Has(start, "cause"),
            "a start has no game, no reason and no cause");

    auto last = s;
    Add(s, Game(3, 1, 0, 10, 179.6f));
    events = Parse(OnlineMatchEvents(&last, s, "R4QE", kBuild, 1761500000));
    Require(events.size() == 1 && events[0]["event"].Text() == "game", "one game ended: its report");
    const JsonValue& game = events[0]["game"];
    Require(game["number"].Integer() == 1 && game["score"]["home"].Integer() == 3
                && game["score"]["away"].Integer() == 1 && game["stadium"].Text() == "the_palace"
                && game["ending"].Text() == "normal" && game["seconds"].Integer() == 180
                && game["captains"]["home"].Text() == "mario",
            "the game's fields");
    Require(events[0]["reason"].IsNull() && !Has(events[0], "cause") && events[0]["series"].Integer() == 1761500000
                && events[0]["build"].Text() == kBuild,
            "a game report: same series and build, no reason or cause");
    Require(!Has(game, "sync"), "a game that ended normally has no sync check");
    Require(OnlineMatchEvents(&s, s, "R4QE", kBuild, 1761500000).empty(), "nothing new, no reports");

    // Two games and the end seen at once.
    last = s;
    Add(s, Game(0, 2, 0, 15));
    Add(s, Game(2, 1, 0, -1));
    events = Parse(OnlineMatchEvents(&last, s, "R4QE", kBuild, 1761500000));
    Require(events.size() == 3 && events[0]["game"]["number"].Integer() == 2
                && events[0]["game"]["stadium"].Text() == "sand_tomb" && events[1]["game"]["number"].Integer() == 3
                && events[1]["game"]["stadium"].Text() == "unknown" && events[2]["event"].Text() == "end"
                && events[2]["reason"].Text() == "decided" && events[2]["cause"].Text() == "decided"
                && events[2]["game"].IsNull() && events[2]["build"].Text() == kBuild,
            "two games and a decided end");
    Require(OnlineMatchEvents(&s, s, "R4QE", kBuild, 1761500000).empty(), "an end is reported once");

    // A new series number begins another: start, games, end at once.
    last = s;
    s = Series(2, 3);
    s.ranked = 0;
    s.side = 1;
    Add(s, Game(1, 2, 4), 4);
    events = Parse(OnlineMatchEvents(&last, s, "R4QP", kBuild, 1761500900));
    Require(events.size() == 3 && events[0]["event"].Text() == "start" && events[0]["mode"].Text() == "friends"
                && events[0]["release"].Text() == "R4QP" && events[0]["series"].Integer() == 1761500900,
            "a new series begins with its start");
    Require(events[0]["side"].Text() == "away" && events[0]["mii"].Text() == "Sam"
                && events[0]["opponent_mii"].Text() == "Anosch",
            "the away side reports its own Mii");
    Require(events[1]["game"]["ending"].Text() == "disconnect" && !Has(events[1]["game"], "sync")
                && events[2]["reason"].Text() == "abandoned" && events[2]["cause"].Text() == "disconnect",
            "a lost connection: disconnect, abandoned for a disconnect");

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
    events = Parse(OnlineMatchEvents(nullptr, s, "R4QE", kBuild, 5));
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
    events = Parse(OnlineMatchEvents(&last, s, "R4QE", kBuild, 6));
    Require(events[0]["game"]["ending"].Text() == "normal" && events[0]["game"]["stadium"].Text() == "unknown"
                && events[0]["game"]["seconds"].Integer() == 0,
            "odd values reported as normal, unknown, 0");
}

// The end report of a series of `bestOf` that `stop` ends.
JsonValue End(int bestOf, void (*stop)(ChargedOnlineMatchSnapshot&))
{
    auto s = Series(1, bestOf);
    const auto last = s;
    stop(s);
    const auto events = Parse(OnlineMatchEvents(&last, s, "R4QE", kBuild, 9));
    Require(!events.empty() && events.back()["event"].Text() == "end", "the series ended");
    return events.back();
}

void TestDiagnostics()
{
    // Why each series ended, beside its reason: decided, out of sync, the
    // connection lost, the online session left with no result (the game
    // module's stop), and a stop the game did not explain.
    auto end = End(1, [](ChargedOnlineMatchSnapshot& s) { Add(s, Game(2, 0)); });
    Require(end["reason"].Text() == "decided" && end["cause"].Text() == "decided", "decided: cause decided");
    end = End(3, [](ChargedOnlineMatchSnapshot& s) { Add(s, Game(1, 1, 2), 2); });
    Require(end["reason"].Text() == "abandoned" && end["cause"].Text() == "desync", "out of sync: cause desync");
    end = End(3, [](ChargedOnlineMatchSnapshot& s) { ChargedOnlineSeriesResult(s, 4, false, nullptr); });
    Require(end["reason"].Text() == "abandoned" && end["cause"].Text() == "disconnect",
            "connection lost: cause disconnect");
    end = End(3, [](ChargedOnlineMatchSnapshot& s) {
        s.ended = 2;
        s.stopped_by = 0;
    });
    Require(end["reason"].Text() == "abandoned" && end["cause"].Text() == "offline", "offline: cause offline");
    end = End(3, [](ChargedOnlineMatchSnapshot& s) {
        s.ended = 2;
        s.stopped_by = 7;
    });
    Require(end["reason"].Text() == "abandoned" && !Has(end, "cause"), "an unexplained stop: no cause");

    // A game out of sync names the check that failed and its frame; a game
    // that ended otherwise, or whose check is unknown, names none.
    const char* checks[] = {"frame", "checksum", "seed"};
    for (int check = 1; check <= 3; ++check)
    {
        auto s = Series(2, 3);
        const auto last = s;
        auto game = Game(1, 0, 2);
        game.sync_check = check;
        game.sync_frame = 4812;
        Add(s, game, 2);
        const auto events = Parse(OnlineMatchEvents(&last, s, "R4QE", kBuild, 10));
        const JsonValue& sync = events[0]["game"]["sync"];
        Require(events[0]["game"]["ending"].Text() == "desync" && sync["check"].Text() == checks[check - 1]
                    && sync["frame"].Integer() == 4812 && sync.Names().size() == 2,
                std::string("a game out of sync: ") + checks[check - 1]);
    }
    const auto gameOf = [](const ChargedOnlineGame& game, int result) {
        auto s = Series(3, 3);
        const auto last = s;
        Add(s, game, result);
        return Parse(OnlineMatchEvents(&last, s, "R4QE", kBuild, 11))[0]["game"];
    };
    auto game = Game(1, 0);
    game.sync_check = 2;
    game.sync_frame = 300;
    Require(!Has(gameOf(game, 0), "sync"), "a normal end has no sync check");
    game.ending = 4;
    Require(!Has(gameOf(game, 4), "sync"), "a lost connection has none");
    game = Game(1, 0, 2);
    Require(Has(gameOf(game, 2), "ending") && !Has(gameOf(game, 2), "sync"), "out of sync, check unknown: none");
    game.sync_check = 4;
    Require(!Has(gameOf(game, 2), "sync"), "an odd check: none");
    game.sync_check = 1;
    game.sync_frame = -1;
    Require(!Has(gameOf(game, 2), "sync"), "no frame: none");

    // The build as the server keeps it: printable, at most 64 characters.
    auto events = Parse(OnlineMatchEvents(nullptr, Series(4, 1), "R4QE", std::string(70, '7') + "\n", 12));
    Require(events[0]["build"].Text() == std::string(64, '7'), "a long build name cut to 64 characters");
    events = Parse(OnlineMatchEvents(nullptr, Series(4, 1), "R4QE", "1.1.0\t+g1\x01", 12));
    Require(events[0]["build"].Text() == "1.1.0+g1", "control characters left out");

    // An error code: its number, the session state then and the profile.
    ChargedOnlineError error{1, 86420, 2, 1000000000};
    const auto shown = ParseJson(OnlineErrorReport(error, "R4QE", kBuild));
    Require(shown["release"].Text() == "R4QE" && shown["build"].Text() == kBuild && shown["code"].Integer() == 86420
                && shown["phase"].Text() == "matchmaking" && shown["profile_id"].Integer() == 1000000000
                && shown.Names().size() == 5,
            "an error code's fields");
    const char* phases[] = {"none", "login", "matchmaking", "prestart", "loading", "in_game", "game_end"};
    for (int phase = 0; phase < 7; ++phase)
    {
        error.phase = phase;
        Require(ParseJson(OnlineErrorReport(error, "R4QP", kBuild))["phase"].Text() == phases[phase],
                std::string("the phase ") + phases[phase]);
    }
    // Unknown: the session state, the profile. Left out.
    error.phase = -1;
    error.profile_id = 0;
    auto unknown = ParseJson(OnlineErrorReport(error, "R4QE", kBuild));
    Require(unknown["code"].Integer() == 86420 && !Has(unknown, "phase") && !Has(unknown, "profile_id"),
            "an unknown phase and profile left out");
    error.phase = 7;
    Require(!Has(ParseJson(OnlineErrorReport(error, "R4QE", kBuild)), "phase"), "an odd phase left out");
    // Only the codes a popup shows, 1 to 999999.
    for (const int code : {0, -5, 1000000})
    {
        error.code = code;
        Require(OnlineErrorReport(error, "R4QE", kBuild).empty(), "no report of code " + std::to_string(code));
    }
    for (const int code : {1, 999999})
    {
        error.code = code;
        Require(ParseJson(OnlineErrorReport(error, "R4QE", kBuild))["code"].Integer() == code,
                "the code " + std::to_string(code));
    }
}

// The fake server: records bodies and their paths, answers from a script of
// results.
struct FakeServer
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> bodies;
    std::vector<std::string> paths;
    std::vector<bool> answers; // per call; true after the script ends
    std::size_t calls = 0;

    OnlineReports::Send Send()
    {
        return [this](const std::string& path, const std::string& body, const std::shared_ptr<HttpCancel>&) {
            std::lock_guard lock(mutex);
            const bool answer = calls < answers.size() ? answers[calls] : true;
            ++calls;
            if (answer)
            {
                bodies.push_back(body);
                paths.push_back(path);
            }
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

// The fake HTTP server behind OnlineReports::HttpSend: records requests,
// answers each path from a script of statuses (0: no answer, the network
// failed; 200 after the script ends).
struct FakeHttp
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<HttpRequest> requests;
    std::map<std::string, std::vector<long>> answers;

    OnlineReports::Request Request()
    {
        return [this](const HttpRequest& request) {
            std::lock_guard lock(mutex);
            requests.push_back(request);
            changed.notify_all();
            auto& script = answers[request.url.substr(request.url.find("/api/"))];
            const long status = script.empty() ? 200 : script.front();
            if (!script.empty()) script.erase(script.begin());
            if (!status) throw HttpError("Connection refused");
            HttpResponse response;
            response.status = status;
            response.body = status == 200 ? "{}" : "{\"error\":\"no\"}";
            return response;
        };
    }

    // Requests to `path` so far.
    std::size_t To(const std::string& path)
    {
        std::lock_guard lock(mutex);
        return std::size_t(std::count_if(requests.begin(), requests.end(), [&](const HttpRequest& request) {
            return request.url.find(path) != std::string::npos;
        }));
    }

    bool WaitFor(const std::string& path, std::size_t count)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return std::size_t(std::count_if(requests.begin(), requests.end(), [&](const HttpRequest& request) {
                       return request.url.find(path) != std::string::npos;
                   }))
                >= count;
        });
    }
};

// The server's answers: what is tried again, what turns an endpoint off.
void TestHttpSend()
{
    const auto cancel = std::make_shared<HttpCancel>();
    std::vector<std::string> log;
    const auto keep = [&](const std::string& line) { log.push_back(line); };

    // A report: tried again after a server error or without an answer, not
    // after a refusal.
    {
        FakeHttp http;
        http.answers[kReports] = {503, 0, 400, 200};
        const auto send = OnlineReports::HttpSend("192.0.2.7", keep, http.Request());
        Require(!send(kReports, "{\"event\":\"start\"}", cancel), "a report answered 503 is tried again");
        Require(!send(kReports, "{}", cancel), "a report without an answer is tried again");
        Require(send(kReports, "{}", cancel), "a refused report is not");
        Require(send(kReports, "{}", cancel), "a stored one is delivered");
        const HttpRequest& first = http.requests[0];
        Require(http.requests.size() == 4 && first.url == "http://192.0.2.7/api/v1/wfc/reports"
                    && first.method == "POST" && first.body == "{\"event\":\"start\"}" && first.allow_http
                    && first.cancel == cancel,
                "a report's request");
        Require(log.size() == 2 && log[0].find("Connection refused") != std::string::npos
                    && log[1].find("refused a report (HTTP 400)") != std::string::npos,
                "the failed network and the refusal logged");
    }

    // An error code: any answer delivers it, only a failed network tries it
    // again.
    {
        log.clear();
        FakeHttp http;
        http.answers[kErrors] = {500, 0, 429, 400, 200};
        const auto send = OnlineReports::HttpSend("wfc.example:8080", keep, http.Request());
        Require(send(kErrors, "{\"code\":1}", cancel), "an error code answered 500 is not tried again");
        Require(!send(kErrors, "{}", cancel), "one without an answer is");
        Require(send(kErrors, "{}", cancel), "one refused (429) is not");
        Require(send(kErrors, "{}", cancel), "one refused (400) is not");
        Require(send(kErrors, "{}", cancel), "a stored one is delivered");
        Require(http.requests.size() == 5 && http.requests[0].url == "http://wfc.example:8080/api/v1/wfc/errors"
                    && http.requests[0].body == "{\"code\":1}",
                "an error code's request");
    }

    // A server from before the error codes (404): the error code is
    // delivered, the next ones are not sent, the reports go on. One without
    // reports (404) sends no more reports, but error codes.
    {
        log.clear();
        FakeHttp http;
        http.answers[kErrors] = {404};
        const auto send = OnlineReports::HttpSend("192.0.2.7", keep, http.Request());
        Require(send(kErrors, "{}", cancel) && http.To(kErrors) == 1, "an error code answered 404 is not tried again");
        Require(send(kErrors, "{}", cancel) && http.To(kErrors) == 1, "nor sent after it");
        Require(send(kReports, "{}", cancel) && http.To(kReports) == 1, "reports go on");
        Require(log.size() == 1 && log[0].find("error codes off") != std::string::npos
                    && log[0].find("HTTP 404") != std::string::npos,
                "error codes off, logged once");

        FakeHttp older;
        older.answers[kReports] = {405};
        const auto sendOlder = OnlineReports::HttpSend("192.0.2.7", nullptr, older.Request());
        Require(sendOlder(kReports, "{}", cancel) && sendOlder(kReports, "{}", cancel) && older.To(kReports) == 1,
                "no more reports after a 405");
        Require(sendOlder(kErrors, "{}", cancel) && older.To(kErrors) == 1, "error codes go on");
    }
}

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
            OnlineReports reports("R4QE", kBuild, server.Send(), [&](const std::string& line) {
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
            Require(std::count(server.paths.begin(), server.paths.end(), kReports) == 3,
                    "reports go to /api/v1/wfc/reports");
        }
        std::lock_guard lock(logMutex);
        Require(log.empty(), "nothing to log when reports get through");
    }

    // Error codes go to their own path, each once, outside a series too; of
    // several since the last look, the last one.
    {
        FakeServer server;
        {
            OnlineReports reports("R4QE", kBuild, server.Send(), nullptr, 10ms);
            ChargedOnlineMatchSnapshot s{};
            s.error = {1, 20110, 1, 0};
            reports.Observe(s);
            reports.Observe(s);
            s.error = {3, 86420, 2, 1000000042};
            reports.Observe(s);
            auto series = Series(1, 3);
            series.error = s.error;
            reports.Observe(series);
            Require(server.WaitFor(3), "two error codes and a start delivered");
            const auto first = ParseJson(server.bodies[0]);
            const auto last = ParseJson(server.bodies[1]);
            Require(server.Calls() == 3 && server.paths[0] == kErrors && server.paths[1] == kErrors
                        && server.paths[2] == kReports,
                    "error codes to /api/v1/wfc/errors, the start to the reports");
            Require(first["code"].Integer() == 20110 && first["phase"].Text() == "login" && !Has(first, "profile_id")
                        && first["release"].Text() == "R4QE" && first["build"].Text() == kBuild,
                    "the first error code");
            Require(last["code"].Integer() == 86420 && last["phase"].Text() == "matchmaking"
                        && last["profile_id"].Integer() == 1000000042,
                    "of two since the last look, the last one");
        }
    }

    // Through HttpSend: an error code answered 404 is not tried again, the
    // next ones not sent, while the reports go through.
    {
        FakeHttp http;
        http.answers[kErrors] = {404};
        std::vector<std::string> log;
        std::mutex logMutex;
        {
            const auto keep = [&](const std::string& line) {
                std::lock_guard lock(logMutex);
                log.push_back(line);
            };
            OnlineReports reports("R4QE", kBuild, OnlineReports::HttpSend("192.0.2.7", keep, http.Request()), keep,
                                  10ms);
            ChargedOnlineMatchSnapshot s = Series(1, 3);
            s.error = {1, 86420, 2, 0};
            reports.Observe(s);
            s.error.shown = 2;
            reports.Observe(s);
            Add(s, Game(1, 0));
            reports.Observe(s);
            // The reports go in order: the game's comes after both error codes.
            Require(http.WaitFor(kReports, 2), "the start and the game delivered");
            Require(http.To(kErrors) == 1, "the error code tried once, the next one not sent");
        }
        std::lock_guard lock(logMutex);
        Require(log.size() == 1 && log[0].find("error codes off") != std::string::npos, "error codes off, logged");
    }

    // A report that never gets through is dropped after four tries; the next goes on.
    {
        FakeServer server;
        server.answers = {false, false, false, false, true};
        std::vector<std::string> log;
        std::mutex logMutex;
        {
            OnlineReports reports("R4QE", kBuild, server.Send(), [&](const std::string& line) {
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
            OnlineReports reports("R4QE", kBuild, [&](const std::string&, const std::string&,
                                                      const std::shared_ptr<HttpCancel>& cancel) {
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
        TestDiagnostics();
        TestHttpSend();
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
