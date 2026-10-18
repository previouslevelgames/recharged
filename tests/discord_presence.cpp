// The Discord status (issue #71): frames and status JSON, the texts made from
// the game's state, and the connection against a fake Discord app (a local
// socket speaking the same frames) for coalescing, clearing and reconnecting.
#include "platform/discord_ipc.h"
#include "platform/discord_presence.h"
#include "platform/game_presence.h"
#include "platform/json.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace mscharged::platform;
namespace fs = std::filesystem;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

ChargedPresenceSnapshot Menu(std::initializer_list<int> scenes)
{
    ChargedPresenceSnapshot s{};
    for (int& scene : s.scenes) scene = -2;
    for (const int scene : scenes) s.scenes[s.scene_count++] = scene;
    s.mode = 0;
    s.game_state = -1;
    s.teams[0] = s.teams[1] = -1;
    s.stadium = -1;
    return s;
}

ChargedPresenceSnapshot Match(int state, float clock)
{
    auto s = Menu({});
    s.in_match = 1;
    s.game_state = state;
    s.teams[0] = 4; // Luigi
    s.teams[1] = 1; // Bowser
    s.stadium = 11; // Thunder Island
    s.scores[0] = 2;
    s.scores[1] = 1;
    s.game_time = clock;
    s.game_duration = 180;
    s.human_sides = 1;
    return s;
}

void FramesAndJson()
{
    std::string stream = discord::EncodeFrame(discord::Opcode::Frame, "{\"a\":1}")
        + discord::EncodeFrame(discord::Opcode::Ping, "{}");
    Require(stream.size() == 8 + 7 + 8 + 2 && stream[0] == 1 && stream[4] == 7, "Frame header layout");
    std::string partial = stream.substr(0, 10);
    Require(!discord::DecodeFrame(partial) && partial.size() == 10, "Incomplete frame was taken");
    const auto first = discord::DecodeFrame(stream);
    const auto second = discord::DecodeFrame(stream);
    Require(first && first->opcode == discord::Opcode::Frame && first->json == "{\"a\":1}", "First frame");
    Require(second && second->opcode == discord::Opcode::Ping && second->json == "{}" && stream.empty(), "Second frame");
    std::string huge = discord::EncodeFrame(discord::Opcode::Frame, "").substr(0, 4) + std::string("\x00\x00\x10\x00", 4);
    bool rejected = false;
    try { discord::DecodeFrame(huge); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "Oversized frame was accepted");

    const auto handshake = ParseJson(discord::HandshakeJson("123456789012345678"));
    Require(handshake["v"].Integer() == 1 && handshake["client_id"].Text() == "123456789012345678", "Handshake");

    discord::Activity activity;
    activity.details = "Quote \" and \\ and\nnewline";
    activity.state = "x"; // too short for Discord: left out
    activity.large_image = "logo";
    activity.large_text = std::string(130, 'a') + "\xC3\xA9"; // clipped to 128 bytes
    activity.start = 1700000000;
    const auto set = ParseJson(discord::SetActivityJson(activity, 4242, "7"));
    Require(set["cmd"].Text() == "SET_ACTIVITY" && set["nonce"].Text() == "7" && set["args"]["pid"].Integer() == 4242,
            "SET_ACTIVITY envelope");
    const auto& shown = set["args"]["activity"];
    Require(shown["details"].Text() == activity.details, "Escaped details");
    Require(shown["state"].IsNull(), "A one-character state was sent");
    Require(shown["timestamps"]["start"].Integer() == 1700000000, "Start time");
    Require(shown["assets"]["large_image"].Text() == "logo" && shown["assets"]["large_text"].Text().size() == 128,
            "Assets and clipping");
    const auto clear = ParseJson(discord::SetActivityJson(std::nullopt, 1, "8"));
    Require(clear["args"]["activity"].IsNull() && clear["args"]["pid"].Integer() == 1, "Clearing the status");

    Require(discord::ValidApplicationId("123456789012345678") && !discord::ValidApplicationId("123")
                && !discord::ValidApplicationId("12345678901234567x8") && !discord::ValidApplicationId(""),
            "Application ID check");
}

void Texts()
{
    GamePresence presence("Mario Strikers Charged", "USA", "R4QE01", 500);
    const std::string game = "Mario Strikers Charged \xC2\xB7 USA";

    auto a = presence.Describe(Menu({10, 1}), 1000); // a popup over the main menu
    Require(a.details == "Main menu" && a.state == game && a.start == 500 && a.large_image == "logo"
                && a.large_text == game + " (R4QE01)",
            "Main menu text: " + a.details + " / " + a.state);
    Require(presence.Describe(Menu({65}), 1000).details == "Hall of Fame", "Hall of Fame");
    Require(presence.Describe(Menu({36, 31}), 1000).state == "Cup Central", "Cup Central");
    Require(presence.Describe(Menu({14, 13}), 1000).details == "Options", "Options");
    const auto rules = presence.Describe(Menu({27, 1}), 1000);
    Require(rules.details == "Domination" && rules.state == "Match options", "Domination's match options");
    Require(presence.Describe(Menu({}), 1000).details == "In the menus", "Empty scene stack");

    auto play = presence.Describe(Match(5, 30), 1000);
    Require(play.details == "Domination \xC2\xB7 Thunder Island" && play.state == "Luigi 2 - 1 Bowser" && play.start == 970,
            "Match text: " + play.details + " / " + play.state + " / " + std::to_string(play.start));
    Require(presence.Describe(Match(5, 31.4f), 1001).start == 970, "Timer moved without drift");
    Require(presence.Describe(Match(2, 39), 1010).start == 970, "Timer moved after a goal");
    Require(presence.Describe(Match(5, 40), 1020).start == 980, "Timer kept the stopped clock's drift");

    auto paused = Match(5, 41);
    paused.scenes[paused.scene_count++] = 80;
    const auto p = presence.Describe(paused, 1030);
    Require(p.state == "Luigi 2 - 1 Bowser (paused)" && p.start == 0, "Paused match");

    auto away = Match(5, 50);
    away.human_sides = 2;
    Require(presence.Describe(away, 1040).state == "Bowser 1 - 2 Luigi", "Away player's team first");

    const auto before = presence.Describe(Match(0, 0), 1050);
    Require(before.state == "Luigi vs Bowser" && before.start == 0, "Before kick-off");
    Require(presence.Describe(Match(-1, 0), 1050).start == 0, "Timer while the match loads");
    const auto over = presence.Describe(Match(3, 180), 1060);
    Require(over.state == "Final score: Luigi 2 - 1 Bowser" && over.start == 0, "Final score");
    Require(presence.Describe(Match(6, 180), 1070).state == "Luigi 2 - 1 Bowser \xC2\xB7 Sudden death", "Sudden death");

    auto demo = Match(5, 10);
    demo.mode = 2;
    demo.scenes[demo.scene_count++] = 0;
    Require(presence.Describe(demo, 1080).details == "Title screen", "Demo match counted as a match");

    auto lesson = Match(5, 10);
    lesson.strikers101 = 1;
    lesson.stadium = 16;
    const auto l = presence.Describe(lesson, 1090);
    Require(l.details == "Strikers 101" && l.state == "In a lesson", "Strikers 101 lesson");

    auto cup = Match(5, 10);
    cup.mode = 3;
    Require(presence.Describe(cup, 1100).details == "Road to the Striker Cup \xC2\xB7 Thunder Island", "Cup match");
    auto ranked = Match(5, 10);
    ranked.online = ranked.ranked = 1;
    Require(presence.Describe(ranked, 1110).details == "Online ranked match \xC2\xB7 Thunder Island", "Ranked match");

    Require(CaptainName(11) && std::string(CaptainName(11)) == "Petey" && !CaptainName(12) && !CaptainName(-1),
            "Captain names");
    Require(StadiumName(16) && std::string(StadiumName(16)) == "The Classroom" && !StadiumName(17), "Stadium names");
}

// A fake Discord app on a Unix socket or a Windows pipe: answers the
// handshake with READY and each SET_ACTIVITY with its nonce, and records
// what it received.
class FakeDiscord
{
public:
    explicit FakeDiscord(std::string endpoint) : endpoint_(std::move(endpoint))
    {
#ifndef _WIN32
        listener_ = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        Require(endpoint_.size() < sizeof address.sun_path, "Socket path too long");
        std::copy(endpoint_.begin(), endpoint_.end(), address.sun_path);
        Require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 && listen(listener_, 4) == 0,
                "Fake Discord cannot listen");
#endif
        thread_ = std::thread([this] { Serve(); });
    }
    ~FakeDiscord()
    {
        stop_ = true;
#ifdef _WIN32
        // Wakes the waiting ConnectNamedPipe.
        const std::wstring name(endpoint_.begin(), endpoint_.end());
        const HANDLE wake = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        thread_.join();
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
#else
        shutdown(listener_, SHUT_RDWR);
        close(listener_);
        thread_.join();
        fs::remove(endpoint_);
#endif
    }

    // Closes the current connection after the next status it receives.
    void DropAfterNextStatus() { drop_ = true; }

    std::vector<std::string> Statuses()
    {
        std::lock_guard lock(mutex_);
        return statuses_;
    }
    int Connections()
    {
        std::lock_guard lock(mutex_);
        return connections_;
    }
    int Pongs()
    {
        std::lock_guard lock(mutex_);
        return pongs_;
    }
    bool WaitFor(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return statuses_.size() >= count; });
    }
    bool WaitForClose(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return closed_ > 0; });
    }

private:
#ifdef _WIN32
    using Client = HANDLE;
    Client Accept()
    {
        const std::wstring name(endpoint_.begin(), endpoint_.end());
        const HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                             PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return nullptr;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            CloseHandle(pipe);
            return nullptr;
        }
        if (stop_)
        {
            CloseHandle(pipe);
            return nullptr;
        }
        return pipe;
    }
    static long Receive(Client client, char* chunk, unsigned size)
    {
        DWORD read = 0;
        return ReadFile(client, chunk, size, &read, nullptr) ? long(read) : -1;
    }
    static void Send(Client client, const std::string& bytes)
    {
        DWORD written = 0;
        WriteFile(client, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    }
    static void Close(Client client)
    {
        FlushFileBuffers(client);
        DisconnectNamedPipe(client);
        CloseHandle(client);
    }
#else
    using Client = int;
    Client Accept()
    {
        const int client = accept(listener_, nullptr, nullptr);
        return client < 0 ? -1 : client;
    }
    static long Receive(Client client, char* chunk, unsigned size) { return long(recv(client, chunk, size, 0)); }
    static void Send(Client client, const std::string& bytes) { send(client, bytes.data(), bytes.size(), MSG_NOSIGNAL); }
    static void Close(Client client) { close(client); }
#endif

    void Serve()
    {
        while (!stop_)
        {
            const Client client = Accept();
#ifdef _WIN32
            if (!client) return;
#else
            if (client < 0) return;
#endif
            {
                std::lock_guard lock(mutex_);
                ++connections_;
            }
            std::string buffer;
            char chunk[4096];
            bool open = true;
            while (open && !stop_)
            {
                const long received = Receive(client, chunk, sizeof chunk);
                if (received <= 0) break;
                buffer.append(chunk, std::size_t(received));
                while (const auto frame = discord::DecodeFrame(buffer))
                {
                    const auto json = ParseJson(frame->json);
                    std::string reply;
                    if (frame->opcode == discord::Opcode::Handshake)
                    {
                        Require(json["client_id"].Text() == "123456789012345678", "Handshake client ID");
                        // A ping before READY: the client must answer it.
                        Send(client, discord::EncodeFrame(discord::Opcode::Ping, "{\"p\":1}"));
                        reply = "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1}}";
                    }
                    else if (frame->opcode == discord::Opcode::Pong)
                    {
                        std::lock_guard lock(mutex_);
                        ++pongs_;
                        continue;
                    }
                    else
                    {
                        {
                            std::lock_guard lock(mutex_);
                            statuses_.push_back(frame->json);
                        }
                        changed_.notify_all();
                        reply = "{\"cmd\":\"SET_ACTIVITY\",\"data\":{},\"evt\":null,\"nonce\":\""
                            + json["nonce"].Text() + "\"}";
                        if (drop_.exchange(false))
                        {
                            open = false;
                            break;
                        }
                    }
                    Send(client, discord::EncodeFrame(discord::Opcode::Frame, reply));
                }
            }
            Close(client);
            {
                std::lock_guard lock(mutex_);
                ++closed_;
            }
            changed_.notify_all();
        }
    }

    std::string endpoint_;
#ifndef _WIN32
    int listener_ = -1;
#endif
    std::atomic<bool> stop_{false}, drop_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> statuses_;
    int connections_ = 0, closed_ = 0, pongs_ = 0;
    std::thread thread_;
};

std::string Details(const std::string& status) { return ParseJson(status)["args"]["activity"]["details"].Text(); }

discord::Activity Named(const char* details)
{
    discord::Activity activity;
    activity.details = details;
    return activity;
}

void Connection(const std::string& endpoint)
{
    using namespace std::chrono_literals;
    std::vector<std::string> log;
    std::mutex log_mutex;
    DiscordPresence::Options options;
    options.application_id = "123456789012345678";
    options.connect = [endpoint] { return discord::ConnectToEndpoint(endpoint); };
    options.log = [&](const std::string& line) {
        std::lock_guard lock(log_mutex);
        log.push_back(line);
    };
    options.min_interval = 300ms;
    options.retry = 100ms;
    options.reply_timeout = 2000ms;

    // Discord not running yet: the game goes on, the status waits.
    auto presence = std::make_unique<DiscordPresence>(options);
    presence->Show(Named("Main menu"));
    std::this_thread::sleep_for(150ms);
    {
        std::lock_guard lock(log_mutex);
        Require(!log.empty() && log.front().find("not running") != std::string::npos, "Missing Discord not logged");
    }

    auto fake = std::make_unique<FakeDiscord>(endpoint);
    Require(fake->WaitFor(1, 3s) && Details(fake->Statuses()[0]) == "Main menu", "First status after Discord started");
    Require(fake->Pongs() == 1, "Ping not answered");
    // Faster changes are merged: only the newest one follows, after the interval.
    const auto first = std::chrono::steady_clock::now();
    presence->Show(Named("Options"));
    presence->Show(Named("Hall of Fame"));
    Require(fake->WaitFor(2, 3s), "Second status not sent");
    Require(std::chrono::steady_clock::now() - first >= 200ms, "Statuses sent faster than the interval");
    std::this_thread::sleep_for(400ms);
    auto statuses = fake->Statuses();
    Require(statuses.size() == 2 && Details(statuses[1]) == "Hall of Fame", "Statuses were not merged");

    // The app drops the connection: the status comes back on a new one.
    fake->DropAfterNextStatus();
    presence->Show(Named("Credits"));
    Require(fake->WaitFor(4, 5s), "Status not sent again after reconnecting");
    statuses = fake->Statuses();
    Require(Details(statuses[2]) == "Credits" && Details(statuses[3]) == "Credits" && fake->Connections() == 2,
            "Reconnect did not resend the status");

    // Leaving clears it.
    presence.reset();
    Require(fake->WaitFor(5, 3s), "Status not cleared on exit");
    statuses = fake->Statuses();
    Require(ParseJson(statuses[4])["args"]["activity"].IsNull(), "Last message did not clear the status");
    Require(fake->WaitForClose(3s), "Connection not closed on exit");
}
}

int main()
{
    try
    {
        FramesAndJson();
        Texts();
        const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
        Connection("\\\\.\\pipe\\recharged-discord-test-" + unique);
#else
        const auto directory = fs::temp_directory_path() / ("recharged-discord-" + unique);
        fs::create_directories(directory);
        try
        {
            Connection((directory / "discord-ipc-0").string());
        }
        catch (...)
        {
            fs::remove_all(directory);
            throw;
        }
        fs::remove_all(directory);
#endif
        std::cout << "Discord status frames, texts, merging, reconnecting and clearing passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
