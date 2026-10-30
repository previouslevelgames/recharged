// The Discord friends' link (src/launcher/discord_friends.cpp) with a fake
// profile server: a kept link is refreshed, a refused one forgotten, a failed
// one kept, and Disconnect forgets it. No trade here succeeds, so the SDK never
// connects to Discord. Without the SDK beside the program the friends stay
// Missing whatever happens.

#include "launcher/discord_friends.h"
#include "platform/profile_service.h"

#include <SDL3/SDL_filesystem.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

using mscharged::launcher::DiscordFriends;
using State = DiscordFriends::State;
namespace platform = mscharged::platform;

namespace
{
int failures = 0;

void Check(bool ok, const char* what, int line)
{
    if (ok) return;
    std::fprintf(stderr, "discord_friends.cpp:%d: %s\n", line, what);
    ++failures;
}
#define CHECK(x) Check((x), #x, __LINE__)

// The trades the friends ask for, answered by the test.
struct FakeServer
{
    std::mutex mutex;
    std::optional<DiscordFriends::TradeRequest> request;
    std::function<void(DiscordFriends::TradeResult)> done;
    int trades = 0;

    DiscordFriends::Trade Trade()
    {
        return [this](DiscordFriends::TradeRequest asked, std::function<void(DiscordFriends::TradeResult)> answer) {
            std::lock_guard<std::mutex> lock(mutex);
            request = std::move(asked);
            done = std::move(answer);
            ++trades;
        };
    }

    // The last request, once it came; the answer goes back later.
    std::optional<DiscordFriends::TradeRequest> Wait()
    {
        for (int i = 0; i < 500; ++i)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (request) return std::exchange(request, std::nullopt);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    }

    void Answer(DiscordFriends::TradeResult result)
    {
        std::function<void(DiscordFriends::TradeResult)> answer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            answer = std::move(done);
        }
        if (answer) answer(std::move(result));
    }
};

DiscordFriends::Status WaitFor(const DiscordFriends& friends, State state)
{
    DiscordFriends::Status status;
    for (int i = 0; i < 500; ++i)
    {
        status = friends.Snapshot();
        if (status.state == state) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return status;
}
} // namespace

int main()
{
    const auto folder = std::filesystem::temp_directory_path()
        / ("recharged-discord-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto link = platform::DiscordLinkFile(folder, "recharged.ini");
    const char* base = SDL_GetBasePath();
    const std::filesystem::path beside = base ? base : ".";
    const DiscordFriends::Account account{"https://wfc.example.com", 1561321462327943259ull};
    FakeServer server;
    {
        DiscordFriends friends(beside, link, server.Trade());
        if (friends.Snapshot().state == State::Missing)
        {
            // No SDK: nothing ever links.
            std::printf("No %s beside the test: only the missing SDK is checked\n", DiscordFriends::LibraryName());
            friends.SetAccount(account);
            friends.Connect();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(friends.Snapshot().state == State::Missing);
            CHECK(server.trades == 0);
        }
        else
        {
            CHECK(friends.Snapshot().state == State::Off);
            // Not linked on this computer.
            friends.SetAccount(account);
            CHECK(WaitFor(friends, State::NotLinked).state == State::NotLinked);

            // A kept link of another server stays unused...
            platform::SaveDiscordLink(link, {"https://other.example.com", "other"});
            friends.SetAccount({});
            CHECK(WaitFor(friends, State::Off).state == State::Off);
            friends.SetAccount(account);
            CHECK(WaitFor(friends, State::NotLinked).state == State::NotLinked);
            CHECK(server.trades == 0);

            // ...the server's own is refreshed; Discord refused it: forgotten.
            platform::SaveDiscordLink(link, {account.server, "r1"});
            friends.SetAccount({});
            WaitFor(friends, State::Off);
            friends.SetAccount(account);
            const auto refresh = server.Wait();
            CHECK(refresh && refresh->refresh_token == "r1" && refresh->code.empty());
            CHECK(friends.Snapshot().state == State::Connecting);
            server.Answer({{}, {}, "Discord refused: connect Discord again", true});
            auto status = WaitFor(friends, State::NotLinked);
            CHECK(status.state == State::NotLinked && !status.error.empty());
            CHECK(!platform::LoadDiscordLink(link));

            // Connect with a kept link uses it; a failure keeps it for later.
            platform::SaveDiscordLink(link, {account.server, "r2"});
            friends.Connect();
            const auto again = server.Wait();
            CHECK(again && again->refresh_token == "r2");
            const auto serial = friends.Snapshot().error_serial;
            server.Answer({{}, {}, "Discord could not be reached", false});
            status = WaitFor(friends, State::Failed);
            CHECK(status.state == State::Failed && status.error == "Discord could not be reached"
                  && status.error_serial != serial);
            CHECK(platform::LoadDiscordLink(link) && platform::LoadDiscordLink(link)->refresh_token == "r2");

            // Disconnect forgets it.
            friends.Disconnect();
            CHECK(WaitFor(friends, State::NotLinked).state == State::NotLinked);
            CHECK(!platform::LoadDiscordLink(link));

            // An answer to a trade given up (signed out meanwhile) is dropped.
            platform::SaveDiscordLink(link, {account.server, "r3"});
            friends.Connect();
            CHECK(server.Wait().has_value());
            friends.SetAccount({});
            CHECK(WaitFor(friends, State::Off).state == State::Off);
            server.Answer({{}, {}, "late", false});
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(friends.Snapshot().state == State::Off);

            // A trade answered after the friends are gone.
            friends.SetAccount(account);
            CHECK(server.Wait().has_value());
        }
    }
    server.Answer({{}, {}, "after the end", false});
    std::filesystem::remove_all(folder);
    if (failures)
    {
        std::fprintf(stderr, "%d Discord friends checks failed\n", failures);
        return 1;
    }
    std::printf("Discord friends checks passed\n");
    return 0;
}
