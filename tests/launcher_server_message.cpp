#include "launcher/server_message.h"

#include <chrono>
#include <cstdio>
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

// Waits up to two seconds for the fetch to finish.
bool WaitDone(const ServerMessageFetch& fetch, std::optional<ServerMessage>* message)
{
    for (int i = 0; i < 400; ++i)
    {
        if (fetch.Done(message)) return true;
        std::this_thread::sleep_for(5ms);
    }
    return fetch.Done(message);
}

void Times()
{
    Require(ParseRfc3339("1970-01-01T00:00:00Z") == 0, "the epoch");
    Require(ParseRfc3339("2026-11-21T23:00:00Z") == 1795302000, "a UTC time");
    Require(ParseRfc3339("2026-11-21T23:00:00.123456Z") == 1795302000, "fractions of a second");
    Require(ParseRfc3339("2026-11-22T00:30:00+01:30") == 1795302000, "an offset ahead of UTC");
    Require(ParseRfc3339("2026-11-21T20:00:00-03:00") == 1795302000, "an offset behind UTC");
    Require(ParseRfc3339("2024-02-29T12:00:00Z") == 1709208000, "a leap day");
    for (const char* bad : {"", "2026-11-21", "2026-11-21T23:00:00", "2026-13-01T00:00:00Z", "2026-11-21T24:00:00Z",
                            "2026-11-21T23:00:00Zjunk", "2026-11-21T23:00:00.Z", "26-11-21T23:00:00Z",
                            "2026-11-21T23:00:00+0100"})
        Require(!ParseRfc3339(bad), std::string("not a time: ") + bad);
}

void Answers()
{
    bool readable = false;
    Require(!ParseServerMessage(R"({"message": null})", &readable) && readable, "no message");
    const auto message = ParseServerMessage(
        R"({"message": {"id": 7, "markdown": "**Restart** at 22:00", "tone": "warning",)"
        R"( "posted_at": "2026-11-21T18:00:00Z", "ends_at": "2026-11-21T23:00:00Z"}})",
        &readable);
    Require(message && readable, "a message");
    Require(message->id == 7 && message->markdown == "**Restart** at 22:00" && message->warning, "its fields");
    Require(message->ends_at == 1795302000, "its end");
    Require(ServerMessageCurrent(*message, 1795301999) && !ServerMessageCurrent(*message, 1795302000),
            "shown until its end");
    const auto lasting = ParseServerMessage(R"({"message": {"id": 8, "markdown": "Hi", "tone": "info", "ends_at": null}})");
    Require(lasting && !lasting->warning && !lasting->ends_at && ServerMessageCurrent(*lasting, 4000000000),
            "a message without an end");
    Require(!ParseServerMessage(R"({"message": {"id": 9, "markdown": "  \n "}})", &readable) && readable,
            "an empty message is none");
    for (const char* bad : {"", "[]", R"({"message": 3})", R"({"message": {"markdown": "x"}})",
                            R"({"message": {"id": 0, "markdown": "x"}})", R"({"message": {"id": 1}})",
                            R"({"message": {"id": 1, "markdown": "x", "ends_at": "tomorrow"}})",
                            R"({"message": {"id": 1, "markdown": "x", "ends_at": 5}})"})
    {
        Require(!ParseServerMessage(bad, &readable) && !readable, std::string("unreadable: ") + bad);
    }
    Require(!ParseServerMessage(R"({"message": {"id": 1, "markdown": ")" + std::string(9000, 'x') + R"("}})"),
            "too long");
}

void Fetches()
{
    // The address, plain http only for a test server.
    {
        std::string url;
        bool http = true;
        std::vector<std::string> log;
        ServerMessageFetch fetch("https://rechargedwfc.net",
                                 [&](const std::string& line) { log.push_back(line); },
                                 [&](const HttpRequest& request) {
                                     url = request.url;
                                     http = request.allow_http;
                                     return HttpResponse{200, R"({"message": {"id": 3, "markdown": "Hello"}})"};
                                 });
        std::optional<ServerMessage> message;
        Require(WaitDone(fetch, &message), "the fetch ends");
        Require(url == "https://rechargedwfc.net/api/v1/message" && !http, "it asks the profile server over https");
        Require(message && message->id == 3, "the message arrives");
        Require(log.size() == 1 && log[0].find("Server message: 3") == 0, "it logs the message");
    }
    {
        bool http = false;
        ServerMessageFetch fetch("http://127.0.0.1:8080", {}, [&](const HttpRequest& request) {
            http = request.allow_http;
            return HttpResponse{200, R"({"message": null})"};
        });
        std::optional<ServerMessage> message;
        Require(WaitDone(fetch, &message) && !message && http, "a test server over http, without a message");
    }
    // An older server, an error and a failed request: no message.
    for (const auto& answer : std::vector<std::function<HttpResponse(const HttpRequest&)>>{
             [](const HttpRequest&) { return HttpResponse{404, "Not Found"}; },
             [](const HttpRequest&) { return HttpResponse{500, "oops"}; },
             [](const HttpRequest&) -> HttpResponse { throw HttpError("connection refused"); },
             [](const HttpRequest&) { return HttpResponse{200, "<html>"}; }})
    {
        std::string line;
        ServerMessageFetch fetch("https://example.org", [&](const std::string& text) { line = text; }, answer);
        std::optional<ServerMessage> message;
        Require(WaitDone(fetch, &message) && !message, "no message after a failure");
        Require(line.find("Server message: ") == 0, "the failure is logged");
    }
    // Destroyed while the request runs: it is cancelled.
    {
        ServerMessageFetch fetch("https://example.org", {}, [](const HttpRequest& request) -> HttpResponse {
            for (int i = 0; i < 400 && !request.cancel->Cancelled(); ++i) std::this_thread::sleep_for(5ms);
            throw HttpError("cancelled", true);
        });
        std::this_thread::sleep_for(20ms);
    }
}
} // namespace

int main()
{
    try
    {
        Times();
        Answers();
        Fetches();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "launcher_server_message: %s\n", error.what());
        return 1;
    }
    std::puts("launcher_server_message: ok");
    return 0;
}
