// The replays' sender (online_replays.h) against a small HTTP server on this
// computer: a live stream goes as its chunks arrive and resumes after a broken
// connection, nothing goes while the match's connection is in trouble or for
// a series the server takes no stream of, and a replay the server asked for
// goes after its game, not while one is played.
#include "platform/live_ring.h"
#include "platform/live_watch.h"
#include "platform/match_replay_stream.h"
#include "platform/online_replays.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mscharged::platform;
using namespace mscharged::replay;
using namespace std::chrono_literals;

namespace
{
int sChecks = 0;
void Require(bool condition, const std::string& message)
{
    ++sChecks;
    if (!condition) throw std::runtime_error(message);
}

bool WaitUntil(const std::function<bool()>& done, std::chrono::milliseconds limit = 10s)
{
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until)
    {
        if (done()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return done();
}

// The game module's side: a ring, and how far the match runs ahead.
LiveRing<1u << 16> sRing;
std::atomic<int> sUnconfirmed{0};
std::atomic<unsigned> sStopped{0};

unsigned Take(void* out, unsigned capacity, unsigned* stopped, int* unconfirmed)
{
    if (stopped) *stopped = sStopped.load();
    if (unconfirmed) *unconfirmed = sUnconfirmed.load();
    return out ? unsigned(sRing.Take(static_cast<std::uint8_t*>(out), capacity)) : 0;
}

// A request as the server read it.
struct Received
{
    std::string head;
    std::string target;
    std::string body;
    bool whole = false; // its body ended as it should
};

// One connection at a time; `answer` decides what to say to a request once
// its head came (nothing: read its body, then ask again with the body).
class FakeServer
{
public:
    using Answer = std::function<std::string(const Received& request, bool body_read)>;

    explicit FakeServer(Answer answer) : answer_(std::move(answer))
    {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "the server binds");
        Require(listen(listener_, 8) == 0, "the server listens");
        socklen_t size = sizeof address;
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }
    ~FakeServer()
    {
        stop_ = true;
        shutdown(listener_, SHUT_RDWR);
        close(listener_);
        thread_.join();
    }
    std::string Address() const { return "127.0.0.1:" + std::to_string(port_); }
    std::vector<Received> Requests()
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    // The server stops reading a request after this many body bytes and
    // drops the connection (once).
    std::atomic<std::size_t> break_after{0};

private:
    void Serve()
    {
        while (!stop_)
        {
            const int connection = accept(listener_, nullptr, nullptr);
            if (connection < 0) return;
            Handle(connection);
            close(connection);
        }
    }

    // Reads more of the connection into `in`; false once it closed.
    static bool More(int connection, std::string& in)
    {
        char buffer[4096];
        const ssize_t got = recv(connection, buffer, sizeof buffer, 0);
        if (got <= 0) return false;
        in.append(buffer, std::size_t(got));
        return true;
    }

    void Handle(int connection)
    {
        std::string in;
        while (in.find("\r\n\r\n") == std::string::npos)
            if (!More(connection, in)) return;
        const std::size_t head_end = in.find("\r\n\r\n");
        const std::string head = in.substr(0, head_end);
        in.erase(0, head_end + 4);
        Received request;
        request.head = head;
        request.target = head.substr(head.find(' ') + 1, head.find(' ', head.find(' ') + 1) - head.find(' ') - 1);
        const bool chunked = head.find("Transfer-Encoding: chunked") != std::string::npos;
        std::string early = answer_(request, false);
        if (!early.empty())
        {
            Keep(request);
            send(connection, early.data(), early.size(), MSG_NOSIGNAL);
            return;
        }
        if (chunked)
            for (;;)
            {
                std::size_t line;
                while ((line = in.find("\r\n")) == std::string::npos)
                    if (!More(connection, in)) return Keep(request);
                const std::size_t size = std::stoul(in.substr(0, line), nullptr, 16);
                while (in.size() < line + 2 + size + 2)
                    if (!More(connection, in)) return Keep(request);
                if (!size)
                {
                    request.whole = true;
                    break;
                }
                request.body += in.substr(line + 2, size);
                in.erase(0, line + 2 + size + 2);
                const std::size_t limit = break_after.load();
                if (limit && request.body.size() >= limit)
                {
                    break_after = 0;
                    return Keep(request);
                }
            }
        else
        {
            const auto field = head.find("Content-Length: ");
            const std::size_t length = field == std::string::npos ? 0 : std::stoul(head.substr(field + 16));
            while (in.size() < length)
                if (!More(connection, in)) return Keep(request);
            request.body = in.substr(0, length);
            request.whole = true;
        }
        Keep(request);
        const std::string answer = answer_(request, true);
        send(connection, answer.data(), answer.size(), MSG_NOSIGNAL);
    }

    void Keep(const Received& request)
    {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
    }

    Answer answer_;
    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::vector<Received> requests_;
    std::thread thread_;
};

std::string Http(int status, const std::string& body)
{
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// The reports' start of series 1792000000 (the module's 1) and the server's answer.
std::shared_ptr<OnlineReplayAnswers> Answers(bool streamed)
{
    auto answers = std::make_shared<OnlineReplayAnswers>();
    answers->NameSeries(1, 1792000000);
    answers->Note("{\"event\":\"start\",\"series\":1792000000,\"profile_id\":1000000001}",
                  streamed ? "{\"match_id\":5,\"stream\":true}" : "{\"match_id\":5}");
    return answers;
}

OnlineReplays::Limits Fast()
{
    OnlineReplays::Limits limits;
    limits.tick = 5ms;
    limits.idle_tick = 5ms;
    limits.retry = std::chrono::seconds(0);
    return limits;
}

void Begin(std::uint32_t stream, std::uint32_t game)
{
    LiveBegin begin;
    begin.module_series = 1;
    begin.game = game;
    begin.profile_id = 1000000001;
    Require(sRing.Put(kLiveBegin, stream, &begin, sizeof begin), "a game's begin fits");
}

void Data(std::uint32_t stream, const std::string& bytes)
{
    for (std::size_t at = 0; at < bytes.size(); at += 1000)
        Require(sRing.Put(kLiveData, stream, bytes.data() + at, std::uint32_t(std::min<std::size_t>(1000, bytes.size() - at))),
                "the data fits");
}

std::string Bytes(std::size_t size, unsigned seed)
{
    std::string bytes(size, '\0');
    for (std::size_t n = 0; n < size; ++n) bytes[n] = char((n * 31 + seed * 7 + n / 977) & 0xFF);
    return bytes;
}

// The server keeps one copy and takes bytes by offset, as the real one does.
struct StreamServer
{
    std::mutex mutex;
    std::string have;
    std::string Answer(const Received& request, bool body_read)
    {
        const auto at = request.target.find("offset=");
        const std::size_t offset = at == std::string::npos ? 0 : std::stoul(request.target.substr(at + 7));
        std::lock_guard lock(mutex);
        if (!body_read)
            return offset > have.size() ? Http(409, "{\"have\":" + std::to_string(have.size()) + "}") : std::string();
        return std::string();
    }
    void Took(const Received& request)
    {
        const auto at = request.target.find("offset=");
        const std::size_t offset = std::stoul(request.target.substr(at + 7));
        std::lock_guard lock(mutex);
        if (offset + request.body.size() > have.size() && offset <= have.size())
            have += request.body.substr(have.size() - offset);
    }
};

void LiveStream()
{
    // Sent as it arrives, whole, with the game's numbers.
    {
        StreamServer stream;
        FakeServer server([&](const Received& request, bool body_read) {
            if (body_read)
            {
                stream.Took(request);
                return Http(200, "{\"match_id\":5,\"bytes\":" + std::to_string(stream.have.size()) + "}");
            }
            return stream.Answer(request, false);
        });
        std::vector<std::string> log;
        std::mutex log_mutex;
        OnlineReplays replays(server.Address(), Answers(true), Take,
                              [&](const std::string& line) {
                                  std::lock_guard lock(log_mutex);
                                  log.push_back(line);
                              },
                              Fast());
        const std::string game = Bytes(30000, 1);
        Begin(1, 1);
        Data(1, game.substr(0, 12000));
        std::this_thread::sleep_for(50ms);
        Data(1, game.substr(12000));
        Require(sRing.Put(kLiveEnd, 1, nullptr, 0), "the end fits");
        Require(WaitUntil([&] { return server.Requests().size() == 1 && server.Requests()[0].whole; }),
                "the game's stream arrives whole");
        const auto request = server.Requests()[0];
        Require(request.target == "/api/v1/wfc/streams?profile_id=1000000001&series=1792000000&game=1&offset=0",
                "for its game: " + request.target);
        Require(request.body == game, "every byte, in order");
        Require(WaitUntil([&] {
                    std::lock_guard lock(log_mutex);
                    return !log.empty() && log.back().find("complete (30000 bytes)") != std::string::npos;
                }),
                "the end logged");
    }

    // A broken connection: sent again from a little before, and the server
    // keeps one copy.
    {
        StreamServer stream;
        FakeServer server([&](const Received& request, bool body_read) {
            if (body_read)
            {
                stream.Took(request);
                return Http(200, "{}");
            }
            return stream.Answer(request, false);
        });
        server.break_after = 9000;
        OnlineReplays replays(server.Address(), Answers(true), Take, nullptr, Fast());
        const std::string game = Bytes(40000, 2);
        Begin(2, 2);
        Data(2, game);
        Require(WaitUntil([&] { return server.Requests().size() >= 1; }), "the first request");
        Require(sRing.Put(kLiveEnd, 2, nullptr, 0), "the end fits");
        Require(WaitUntil([&] {
                    const auto requests = server.Requests();
                    return requests.size() >= 2 && requests.back().whole;
                }),
                "the stream goes again after the break");
        {
            const auto first = server.Requests()[0];
            stream.Took(first); // what came before the break stays
        }
        Require(stream.have == game, "the server has the whole game once");
    }

    // A game with nothing new for longer than a stall (a pause, a cutscene)
    // goes on.
    {
        StreamServer stream;
        FakeServer server([&](const Received& request, bool body_read) {
            if (body_read)
            {
                stream.Took(request);
                return Http(200, "{}");
            }
            return stream.Answer(request, false);
        });
        auto limits = Fast();
        limits.stall = std::chrono::seconds(1);
        OnlineReplays replays(server.Address(), Answers(true), Take, nullptr, limits);
        const std::string game = Bytes(6000, 7);
        Begin(7, 1);
        Data(7, game.substr(0, 3000));
        std::this_thread::sleep_for(1500ms);
        Data(7, game.substr(3000));
        Require(sRing.Put(kLiveEnd, 7, nullptr, 0), "the end fits");
        Require(WaitUntil([&] { return server.Requests().size() == 1 && server.Requests()[0].whole; }),
                "a quiet moment is no stall");
        Require(server.Requests()[0].body == game, "the whole game");
    }

    // The match's connection in trouble: nothing goes until it recovers.
    {
        StreamServer stream;
        FakeServer server([&](const Received& request, bool body_read) {
            if (body_read)
            {
                stream.Took(request);
                return Http(200, "{}");
            }
            return stream.Answer(request, false);
        });
        sUnconfirmed = 30;
        OnlineReplays replays(server.Address(), Answers(true), Take, nullptr, Fast());
        Begin(3, 1);
        Data(3, Bytes(5000, 3));
        std::this_thread::sleep_for(150ms);
        Require(server.Requests().empty(), "nothing while the match runs ahead of its inputs");
        sUnconfirmed = 2;
        Require(sRing.Put(kLiveEnd, 3, nullptr, 0), "the end fits");
        Require(WaitUntil([&] { return server.Requests().size() == 1 && server.Requests()[0].whole; }),
                "sent once the inputs caught up");
    }

    // A series the server streams nothing of: nothing goes.
    {
        FakeServer server([](const Received&, bool) { return Http(200, "{}"); });
        OnlineReplays replays(server.Address(), Answers(false), Take, nullptr, Fast());
        Begin(4, 1);
        Data(4, Bytes(5000, 4));
        Require(sRing.Put(kLiveEnd, 4, nullptr, 0), "the end fits");
        std::this_thread::sleep_for(150ms);
        Require(server.Requests().empty(), "no stream the server did not ask for");
    }

    // The module stopped the stream (its ring was full): given up, quietly
    // for the match.
    {
        FakeServer server([](const Received&, bool) { return std::string(); });
        std::vector<std::string> log;
        std::mutex log_mutex;
        OnlineReplays replays(server.Address(), Answers(true), Take,
                              [&](const std::string& line) {
                                  std::lock_guard lock(log_mutex);
                                  log.push_back(line);
                              },
                              Fast());
        Begin(5, 1);
        Data(5, Bytes(2000, 5));
        std::this_thread::sleep_for(100ms); // under way
        sStopped = 5;
        Require(WaitUntil([&] {
                    std::lock_guard lock(log_mutex);
                    return !log.empty() && log.back().find("could not keep up") != std::string::npos;
                }),
                "a stream the module stopped is given up");
        sStopped = 0;
    }
}

void Replays()
{
    const auto folder = std::filesystem::temp_directory_path()
        / ("online_replays_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto file = folder / "replay_20261123-120000.msc";
    const std::string bytes = Bytes(70000, 9);
    {
        std::ofstream out(file, std::ios::binary);
        out.write(bytes.data(), std::streamsize(bytes.size()));
    }
    FakeServer server([](const Received&, bool body_read) { return body_read ? Http(200, "{\"match_id\":5}") : ""; });
    std::vector<std::string> log;
    std::mutex log_mutex;
    auto answers = Answers(false);
    OnlineReplays replays(server.Address(), answers, Take,
                          [&](const std::string& line) {
                              std::lock_guard lock(log_mutex);
                              log.push_back(line);
                          },
                          Fast());
    // Game 2 is being played: game 1's replay waits for its end.
    Begin(6, 2);
    answers->Note("{\"event\":\"game\",\"series\":1792000000,\"profile_id\":1000000001,\"game\":{\"number\":1}}",
                  "{\"match_id\":5,\"replay\":true}");
    replays.Kept(1792000000, 1, file);
    std::this_thread::sleep_for(150ms);
    Require(server.Requests().empty(), "no replay while a game is played");
    Require(sRing.Put(kLiveEnd, 6, nullptr, 0), "the end fits");
    Require(WaitUntil([&] { return server.Requests().size() == 1 && server.Requests()[0].whole; }),
            "the replay goes after the game");
    const auto request = server.Requests()[0];
    Require(request.target == "/api/v1/wfc/replays?profile_id=1000000001&series=1792000000&game=1",
            "for its game: " + request.target);
    Require(request.body == bytes, "the file, whole");
    Require(WaitUntil([&] {
                std::lock_guard lock(log_mutex);
                return !log.empty() && log.back() == "Replays: sent game 1 of series 1792000000";
            }),
            "logged");
    std::filesystem::remove_all(folder);
}
// What the game module took of a live replay.
std::mutex sFedMutex;
std::string sFed;
bool sFedComplete = false;
int Feed(const void* data, unsigned size, int complete)
{
    std::lock_guard lock(sFedMutex);
    sFed.append(static_cast<const char*>(data), size);
    if (complete) sFedComplete = true;
    return 1;
}

std::string Frame(std::uint8_t kind, const std::string& payload)
{
    std::string frame(1, char(kind));
    for (int n = 0; n < 4; ++n) frame += char(std::uint32_t(payload.size()) >> (8 * n));
    return frame + payload;
}

std::string GameNumber(std::uint32_t game)
{
    std::string bytes;
    for (int n = 0; n < 4; ++n) bytes += char(game >> (8 * n));
    return bytes;
}

// A chunked answer of `body`, cut into chunks of odd sizes.
std::string Chunked(const std::string& body)
{
    std::string out = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (std::size_t at = 0, size = 3; at < body.size(); at += size, size = size * 3 % 97 + 1)
    {
        const std::string part = body.substr(at, size);
        char head[16];
        std::snprintf(head, sizeof head, "%zx\r\n", part.size());
        out += head + part + "\r\n";
    }
    return out + "0\r\n\r\n";
}

void Watching()
{
    // A ticket names its match; other files are none.
    std::string header = "{\"match\": 12, \"ticket\": \"00ff\", \"release\": \"R4QE\"}";
    std::vector<std::uint8_t> file = {'M', 'S', 'C', 'R', 'P', 'L', 'Y', 0x1A, 2, 0,
                                      std::uint8_t(header.size()), 0};
    file.insert(file.end(), header.begin(), header.end());
    WatchTicket ticket;
    std::string error;
    Require(ReadWatchTicket(file, ticket, error) && ticket.match == 12 && ticket.ticket == "00ff"
                && ticket.release == "R4QE",
            "a ticket reads");
    file[8] = 1;
    Require(!ReadWatchTicket(file, ticket, error) && error.empty(), "a replay file is no ticket");
    file[8] = 2;
    file.resize(20);
    Require(!ReadWatchTicket(file, ticket, error) && !error.empty(), "a ticket cut short is refused");

    // The stream's start, as the game module writes it.
    mscharged::replay::Setup setup;
    setup.machine_count = 2;
    setup.player_counts[0] = setup.player_counts[1] = 1;
    mscharged::replay::StreamWriter writer;
    Require(writer.Begin(setup), "a setup");
    mscharged::replay::Bytes start;
    writer.StartBytes(start);
    const std::string begin(start.begin(), start.end());
    const std::string steps = Bytes(5000, 11);

    // Joined in game 2: its start, the rest as it comes, its end, then game 3
    // begins and the match ends.
    {
        const std::string body = Frame(0, "") + Frame(1, GameNumber(2)) + Frame(2, begin.substr(0, 10))
            + Frame(2, begin.substr(10)) + Frame(2, steps.substr(0, 2000)) + Frame(0, "")
            + Frame(2, steps.substr(2000)) + Frame(3, "") + Frame(1, GameNumber(3)) + Frame(2, "next game's");
        std::string target, head;
        FakeServer server([&](const Received& request, bool) {
            target = request.target;
            head = request.head;
            return Chunked(body);
        });
        WatchTicket watch;
        watch.match = 12;
        watch.ticket = "00ff";
        LiveWatch live(server.Address(), watch, nullptr);
        int game = 0;
        std::vector<std::uint8_t> got;
        Require(live.WaitForGame(5s, game, got, error), "the game's start: " + error);
        Require(game == 2 && got.size() >= begin.size()
                    && std::string(got.begin(), got.begin() + std::ptrdiff_t(begin.size())) == begin,
                "game 2 from its first byte");
        Require(target == "/api/v1/wfc/streams/12" && head.find("\r\nAuthorization: Bearer 00ff") != std::string::npos,
                "the ticket's match, the ticket out of the URL: " + target);
        live.Feed(Feed);
        Require(WaitUntil([&] { return live.Over(); }), "the match's end");
        std::lock_guard lock(sFedMutex);
        Require(std::string(got.begin(), got.end()) + sFed == begin + steps, "every byte of the game, once");
        Require(sFedComplete && live.NextGame() && !live.Failed(), "its end, and the next game begun");
    }

    // A ticket the server does not take.
    {
        FakeServer server([](const Received&, bool) {
            return Http(403, "{\"error\":\"the ticket does not let you watch this match\"}");
        });
        WatchTicket watch;
        watch.match = 12;
        watch.ticket = "00ff";
        LiveWatch live(server.Address(), watch, nullptr);
        int game = 0;
        std::vector<std::uint8_t> got;
        Require(!live.WaitForGame(5s, game, got, error) && live.Failed()
                    && error.find("four hours") != std::string::npos,
                "a refused ticket says why: " + error);
    }
}
} // namespace

int main()
{
    try
    {
        LiveStream();
        Replays();
        Watching();
    }
    catch (const std::exception& error)
    {
        std::cerr << "online_replays: FAIL after " << sChecks << " checks: " << error.what() << '\n';
        return 1;
    }
    std::cout << "online_replays: " << sChecks << " checks passed\n";
    return 0;
}
