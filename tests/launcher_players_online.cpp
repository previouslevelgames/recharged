#include "launcher/players_online.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mscharged::launcher;
using namespace std::chrono_literals;
using mscharged::platform::HttpError;
using mscharged::platform::HttpRequest;
using mscharged::platform::HttpResponse;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// Waits up to two seconds for `done`.
template <typename Done> bool WaitFor(Done done)
{
    for (int i = 0; i < 400; ++i)
    {
        if (done()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return done();
}

// A server that answers from a script, one answer per ask (the last one
// again once the script ends), and counts the asks.
struct FakeServer
{
    std::vector<std::function<HttpResponse(const HttpRequest&)>> answers;
    std::atomic<int> asks{0};
    std::mutex mutex;
    std::string url; // of the last ask
    bool allow_http = false;

    PlayersOnline::Request Request()
    {
        return [this](const HttpRequest& request) {
            const int ask = asks++;
            {
                std::lock_guard lock(mutex);
                url = request.url;
                allow_http = request.allow_http;
            }
            return answers[std::min<std::size_t>(std::size_t(ask), answers.size() - 1)](request);
        };
    }
};

auto Answer(long status, std::string body)
{
    return [status, body](const HttpRequest&) { return HttpResponse{status, body}; };
}

void Parsing()
{
    auto count = ParsePlayersOnline(R"({"wfc": 12, "playing": 4, "recharged": 3})");
    Require(count && count->online == 12 && count->playing == 4, "The server's answer is read");
    count = ParsePlayersOnline(R"({"playing": 0, "wfc": 0})");
    Require(count && count->online == 0 && count->playing == 0, "Nobody online is an answer");
    Require(!ParsePlayersOnline(""), "An empty body is no answer");
    Require(!ParsePlayersOnline("<html>Not Found</html>"), "HTML is no answer");
    Require(!ParsePlayersOnline("[12, 4]"), "An array is no answer");
    Require(!ParsePlayersOnline(R"({"wfc": 12})"), "A missing number is no answer");
    Require(!ParsePlayersOnline(R"({"wfc": "12", "playing": 4})"), "A string is no number");
    Require(!ParsePlayersOnline(R"({"wfc": -1, "playing": 0})"), "A negative count is no answer");
    Require(!ParsePlayersOnline(R"({"wfc": 1.5, "playing": 0})"), "A fraction is no count");
    Require(!ParsePlayersOnline(R"({"wfc": 2, "playing": 3})"), "More playing than online is no answer");
    Require(!ParsePlayersOnline(R"({"error": "the server's database failed"})"), "An error is no answer");
    Require(PlayersOnlineUrl("2.31.48.136") == "http://2.31.48.136/api/v1/online", "The address");
}

void AsksAndAsksAgain()
{
    FakeServer server;
    server.answers = {Answer(200, R"({"wfc": 12, "playing": 4, "recharged": 0})"),
                      Answer(200, R"({"wfc": 13, "playing": 5, "recharged": 0})")};
    std::vector<std::string> log;
    std::mutex log_mutex;
    PlayersOnline online("192.0.2.7", [&](const std::string& line) {
        std::lock_guard lock(log_mutex);
        log.push_back(line);
    }, 20ms, server.Request());
    Require(WaitFor([&] { return online.Snapshot().has_value(); }), "The first answer is shown");
    {
        std::lock_guard lock(server.mutex);
        Require(server.url == "http://192.0.2.7/api/v1/online" && server.allow_http, "Plain http to online.server");
    }
    Require(WaitFor([&] {
                const auto count = online.Snapshot();
                return count && count->online == 13 && count->playing == 5;
            }),
            "The count follows the server");
    Require(WaitFor([&] { return server.asks >= 4; }), "Asked again and again");
    std::lock_guard lock(log_mutex);
    Require(log.size() == 1 && log[0] == "Players online: from 192.0.2.7", "One log line for the answers");
}

void FailureShowsNothing()
{
    FakeServer server;
    server.answers = {Answer(200, R"({"wfc": 3, "playing": 0})"),
                      [](const HttpRequest&) -> HttpResponse { throw HttpError("Could not connect"); },
                      Answer(503, "Service Unavailable"), Answer(200, R"({"wfc": 2, "playing": 2})")};
    std::vector<std::string> log;
    std::mutex log_mutex;
    PlayersOnline online("192.0.2.7", [&](const std::string& line) {
        std::lock_guard lock(log_mutex);
        log.push_back(line);
    }, 20ms, server.Request());
    Require(WaitFor([&] { return online.Snapshot().has_value(); }), "The first answer is shown");
    Require(WaitFor([&] { return server.asks >= 2 && !online.Snapshot(); }), "A failed ask shows nothing");
    Require(WaitFor([&] {
                const auto count = online.Snapshot();
                return server.asks >= 4 && count && count->online == 2;
            }),
            "The next answer is shown again");
    std::lock_guard lock(log_mutex);
    Require(log.size() == 3 && log[0] == "Players online: from 192.0.2.7"
                && log[1] == "Players online: not available (Could not connect)"
                && log[2] == "Players online: from 192.0.2.7",
            "The failure is logged once, and the way back");
}

void ServerWithoutCount()
{
    for (const long status : {404L, 405L})
    {
        FakeServer server;
        server.answers = {Answer(status, "Not Found")};
        std::string logged;
        std::mutex log_mutex;
        PlayersOnline online("192.0.2.7", [&](const std::string& line) {
            std::lock_guard lock(log_mutex);
            logged += line;
        }, 5ms, server.Request());
        Require(WaitFor([&] { return server.asks >= 1; }), "Asked once");
        std::this_thread::sleep_for(60ms);
        Require(server.asks == 1, "A server without the count is not asked again");
        Require(!online.Snapshot(), "A server without the count shows nothing");
        std::lock_guard lock(log_mutex);
        Require(logged == "Players online: off, the server has no count (HTTP " + std::to_string(status) + ")",
                "Logged: " + logged);
    }
}

void StopsWhileAsking()
{
    // The launcher closes during a slow ask: the destructor cancels it.
    std::atomic<bool> asked{false};
    {
        PlayersOnline online("192.0.2.7", {}, 1min, [&](const HttpRequest& request) -> HttpResponse {
            asked = true;
            while (!request.cancel->Cancelled()) std::this_thread::sleep_for(1ms);
            throw HttpError("cancelled", true);
        });
        Require(WaitFor([&] { return asked.load(); }), "Asking");
    }
    // Waiting between asks: the destructor does not wait the minute out.
    const auto start = std::chrono::steady_clock::now();
    {
        FakeServer server;
        server.answers = {Answer(200, R"({"wfc": 1, "playing": 0})")};
        PlayersOnline online("192.0.2.7", {}, 1min, server.Request());
        Require(WaitFor([&] { return online.Snapshot().has_value(); }), "Answered");
    }
    Require(std::chrono::steady_clock::now() - start < 5s, "Closing does not wait for the next ask");
}
} // namespace

int main()
{
    try
    {
        Parsing();
        AsksAndAsksAgain();
        FailureShowsNothing();
        ServerWithoutCount();
        StopsWhileAsking();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "launcher_players_online: %s\n", error.what());
        return 1;
    }
    std::puts("launcher_players_online: all checks passed");
    return 0;
}
