// The profile service client of competitive play: server addresses, the
// saved session, the rules the game module gets, and (where the system has
// libcurl) the requests themselves against a small HTTP server here.
#include "platform/competitive_rules.h"
#include "platform/http_client.h"
#include "platform/profile_service.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

void Addresses()
{
    CHECK(ProfileServerAddress("https://wfc.example.com") == "https://wfc.example.com");
    CHECK(ProfileServerAddress("  HTTPS://WFC.Example.com/ ") == "https://wfc.example.com");
    CHECK(ProfileServerAddress("https://wfc.example.com:443") == "https://wfc.example.com");
    CHECK(ProfileServerAddress("https://wfc.example.com:8443") == "https://wfc.example.com:8443");
    for (const char* local : {"http://127.0.0.1:8080", "http://localhost:8080", "http://[::1]:8080",
                              "http://192.168.1.20:8080", "http://10.0.0.5", "http://172.16.0.1",
                              "http://169.254.3.4", "http://[fd00::1]:8080"})
        CHECK(ProfileServerAddress(local) == std::string(local));
    CHECK(ProfileServerAddress("http://127.0.0.1:80") == "http://127.0.0.1");
    std::string error;
    for (const char* refused : {"http://wfc.example.com", "http://203.0.113.10:8080", "http://172.32.0.1",
                                "http://[2001:db8::1]", "https://wfc.example.com/api", "https://wfc.example.com/?x=1",
                                "https://user@wfc.example.com", "wfc.example.com", "ftp://wfc.example.com",
                                "https://", "https://wfc.example.com:0", "https://wfc.example.com:65536",
                                "https://wfc.example.com:x", "http://127.0.0.1.5", "http://256.0.0.1"})
    {
        error.clear();
        CHECK(!ProfileServerAddress(refused, &error));
        CHECK(!error.empty());
    }
}

void Sessions()
{
    const auto folder = std::filesystem::temp_directory_path()
        / ("recharged-session-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(folder);
    const auto file = ProfileSessionFile(folder, "/games/recharged/second.ini");
    CHECK(file == folder / "online" / "second.session");
    CHECK(!LoadProfileSession(file));
    SaveProfileSession(file, {"https://wfc.example.com", "token \"quoted\"\n", "Nelly"});
    const auto loaded = LoadProfileSession(file);
    CHECK(loaded && loaded->server == "https://wfc.example.com" && loaded->token == "token \"quoted\"\n"
          && loaded->player_name == "Nelly");
#ifndef _WIN32
    struct stat info{};
    CHECK(::stat(file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
#endif
    SaveProfileSession(file, {"https://other.example.com", "second", "Sam"});
    CHECK(LoadProfileSession(file)->token == "second");
    { std::ofstream(file) << "not json"; }
    CHECK(!LoadProfileSession(file));
    ForgetProfileSession(file);
    CHECK(!std::filesystem::exists(file));
    std::filesystem::remove_all(folder);
}

void Rules()
{
    MatchRules rules;
    rules.tweaks = {"fast_terrain", "no_mega_strikes", "thunder_island_walls"};
    rules.skill = "superstar";
    rules.limit = "goals";
    rules.time = 180;
    rules.goals = 3;
    rules.games = 5;
    rules.stadium = "the_palace";
    ProfilePlayer opponent;
    opponent.wfc = {{1000000001, "R4QE", "", ""}, {1000000002, "R4QP", "", ""}};
    const auto module = ModuleRules(rules, opponent);
    CHECK(module && module->tweaks == ((1u << 2) | (1u << 5) | (1u << 7)) && module->skill == 3 && module->limit == 1
          && module->goals == 3 && module->games == 5 && module->stadium == 10 && module->opponent_count == 2
          && module->opponents[1] == 1000000002u);
    CHECK(DescribeRules(rules)
          == "Superstar, first to 3 goals, best of 5, The Palace, Fast terrain, No Mega Strikes, Thunder Island walls");
    rules.limit = "time";
    rules.time = 150;
    rules.games = 1;
    rules.stadium = "random";
    rules.tweaks.clear();
    CHECK(DescribeRules(rules) == "Superstar, 2:30 minutes, one game, random stadium");
    CHECK(ModuleRules(rules, opponent)->stadium == -1);
    std::string error;
    rules.tweaks = {"super_team"};
    CHECK(!ModuleRules(rules, opponent, &error) && !error.empty());
    rules.tweaks.clear();
    rules.stadium = "tutorial_field";
    CHECK(!ModuleRules(rules, opponent));
    CHECK(CaptainKey(0) == "mario" && CaptainKey(3) == "donkey_kong" && CaptainKey(11) == "petey" && CaptainKey(12).empty()
          && CaptainKey(-1).empty());
    CHECK(StadiumKey(0) == "battle_dome" && StadiumKey(15) == "sand_tomb" && StadiumKey(16).empty());
    CHECK(NameLabel("bowser_jr") == "Bowser Jr");
}

#ifndef _WIN32
// One exchange the little server expects, and its answer.
struct Exchange
{
    std::string request; // "POST /api/v1/matches"
    int status;
    std::string answer;
};

// What the client sent.
struct Sent
{
    std::string body, authorization, content_type, activity;
    bool expected = false;
};

// Serves `exchanges` in order on 127.0.0.1, one connection each.
class Server
{
public:
    Server(const std::vector<Exchange>& exchanges, std::vector<Sent>& sent) : exchanges_(exchanges), sent_(sent)
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof address;
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(socket_, 4) != 0
            || ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0)
            throw std::runtime_error("the test server cannot listen");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Serve(); });
    }
    ~Server()
    {
        thread_.join();
        ::close(socket_);
    }
    std::string Url() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
    void Serve()
    {
        for (const auto& exchange : exchanges_)
        {
            auto& sent = sent_.emplace_back();
            const int client = ::accept(socket_, nullptr, nullptr);
            if (client < 0) return;
            std::string text;
            char buffer[4096];
            std::size_t headers = std::string::npos, length = 0;
            while (true)
            {
                if (headers == std::string::npos && (headers = text.find("\r\n\r\n")) != std::string::npos)
                {
                    const auto found = text.find("Content-Length: ");
                    if (found != std::string::npos && found < headers) length = std::stoul(text.substr(found + 16));
                }
                if (headers != std::string::npos && text.size() >= headers + 4 + length) break;
                const auto got = ::recv(client, buffer, sizeof buffer, 0);
                if (got <= 0) break;
                text.append(buffer, std::size_t(got));
            }
            const auto line = text.substr(0, text.find(" HTTP/1.1"));
            auto header = [&](const char* name) {
                const auto at = text.find(name);
                return at == std::string::npos || at > headers
                    ? std::string() : text.substr(at + std::strlen(name), text.find("\r\n", at) - at - std::strlen(name));
            };
            sent.body = headers == std::string::npos ? std::string() : text.substr(headers + 4);
            sent.authorization = header("Authorization: ");
            sent.content_type = header("Content-Type: ");
            sent.activity = header("Recharged-Activity: ");
            sent.expected = line == exchange.request;
            const std::string answer = sent.expected ? exchange.answer : "{\"error\":\"unexpected " + line + "\"}";
            const std::string reply = "HTTP/1.1 " + std::to_string(sent.expected ? exchange.status : 599)
                + " X\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(answer.size())
                + "\r\nConnection: close\r\n\r\n" + answer;
            ::send(client, reply.data(), reply.size(), 0);
            ::close(client);
        }
    }

    const std::vector<Exchange>& exchanges_;
    std::vector<Sent>& sent_;
    int socket_ = -1;
    int port_ = 0;
    std::thread thread_;
};

const char* kMatch = R"({"id":12,"state":"accepted","side":"challenger","opponent":{"id":8,"name":"Sam","avatar_url":null,
"discord":"sam","wfc":[{"profile_id":1000000001,"game":"R4QE","friend_code":"0868-9934-5920","mii":"Sam"}]},
"rule_set":"Competitive","rules":{"tweaks":["no_mega_strikes"],"skill":"rookie","limit":"time","time":180,"goals":5,
"games":3,"stadium":"random"},"rules_hash":"abc","paired":false,"reported":false,"winner":null,"games":[],"created_at":1})";
// The same set on a server with sets: both ready, go.
const char* kSet = R"({"id":12,"state":"accepted","side":"challenger","opponent":{"id":8,"name":"Sam","discord":"sam"},
"rule_set":"Competitive","rules":{"tweaks":[],"skill":"rookie","limit":"time","time":180,"goals":5,"games":3,
"stadium":"random"},"rules_hash":"abc","paired":false,"reported":false,"winner":null,"games":[],"created_at":1,
"stage":"go","reason":null,"you_ready":true,"opponent_ready":true,"opponent_online":true,"go_at":1792771200,
"playing_at":null,"rematch_of":11})";

// The friends list's wording of when a player last had Recharged open.
void LastOnline()
{
    const std::int64_t now = 1793300000;
    CHECK(DescribeLastOnline(now, std::nullopt) == "Not online yet");
    CHECK(DescribeLastOnline(now, now - 30) == "Last online just now");
    CHECK(DescribeLastOnline(now, now + 300) == "Last online just now");
    CHECK(DescribeLastOnline(now, now - 60 * 60 + 1) == "Last online 59 minutes ago");
    CHECK(DescribeLastOnline(now, now - 60 * 60) == "Last online 1 hour ago");
    CHECK(DescribeLastOnline(now, now - 5 * 60 * 60) == "Last online 5 hours ago");
    CHECK(DescribeLastOnline(now, now - 30 * 60 * 60) == "Last online yesterday");
    CHECK(DescribeLastOnline(now, now - 3 * 86400) == "Last online 3 days ago");
    const auto longer = DescribeLastOnline(now, now - 40 * 86400);
    CHECK(longer.rfind("Last online on ", 0) == 0 && longer.find("ago") == std::string::npos);
    CHECK(DescribeLastOnline(now, now - 400 * 86400).find("202") != std::string::npos);

    // Chat times: the clock today, then "Yesterday", then the day.
    const auto today = DescribeChatTime(now, now);
    CHECK(today.size() == 5 && today[2] == ':');
    CHECK(DescribeChatTime(now, now - 86400).rfind("Yesterday ", 0) == 0);
    const auto earlier = DescribeChatTime(now, now - 40 * 86400);
    CHECK(earlier.find(", ") != std::string::npos && earlier.find("202") == std::string::npos);
    CHECK(DescribeChatTime(now, now - 400 * 86400).find("202") != std::string::npos);
}

void Requests()
{
    if (!HttpUnavailable().empty())
    {
        std::printf("HTTP requests skipped: %s\n", HttpUnavailable().c_str());
        return;
    }
    const std::vector<Exchange> exchanges{
        {"GET /api/v1/server", 200, R"({"version":"1.0.4","sign_in":{"discord":true,"test":false}})"},
        {"POST /api/v1/sign-in/test", 200,
         R"({"status":"signed_in","token":"t0k3n","player":{"id":7,"name":"Nelly","discord":"nelly","test":true,"wfc":[]}})"},
        {"POST /api/v1/matches", 201, kMatch},
        {"POST /api/v1/matches/12/ready", 200, R"({"paired":true,"other_release":false})"},
        {"POST /api/v1/matches/12/report", 200, kMatch},
        {"POST /api/v1/matches/12/accept", 409, R"({"error":"the match is accepted already"})"},
        {"GET /api/v1/matches/current", 200, R"({"match":null})"},
        {"GET /api/v1/me", 401, R"({"error":"signed out"})"},
        {"GET /api/v1/players", 200,
         R"({"online":[{"id":8,"name":"Sam","discord":"sam","test":false,"online":true,"busy":false,)"
         R"("activity":"match","last_online":1793000000,"friend":false}],"recent":[],)"
         R"("friends":[{"id":9,"name":"Bo","online":false,"busy":false,"last_online":null,"friend":true}]})"},
        {"POST /api/v1/matches", 201, kSet},
        {"POST /api/v1/matches/12/ready-to-play", 200, kSet},
        {"POST /api/v1/matches/12/rematch", 200, kSet},
        {"GET /api/v1/matches/current", 200, std::string(R"({"poll_interval":2,"server_time":5,"match":)") + kSet + "}"},
        {"GET /api/v1/me", 200,
         R"({"id":7,"name":"Nelly","discord":"nelly","test":false,"wfc":[],"discord_id":"80351110224678912"})"},
        {"POST /api/v1/friends", 200, R"({"friends":[{"id":8,"name":"Sam","online":true,"friend":true}]})"},
        {"POST /api/v1/friends", 200, R"({"friends":[{"id":8,"name":"Sam"},{"id":9,"name":"Bo"}]})"},
        {"DELETE /api/v1/friends/8", 200, R"({"friends":[{"id":9,"name":"Bo","chat":true,"unread":2}]})"},
        {"GET /api/v1/chat/9?after=5", 200,
         R"({"messages":[{"id":6,"mine":false,"body":"hi","sent_at":1793000100}],"can_send":true})"},
        {"POST /api/v1/chat/9", 201, R"({"id":7,"mine":true,"body":"yo \"x\"","sent_at":1793000200})"},
    };
    std::vector<Sent> sent;
    {
        Server server(exchanges, sent);
        ProfileService api(server.Url());
        const auto info = api.Info();
        CHECK(info.version == "1.0.4" && info.discord && !info.test);
        const auto signed_in = api.TestSignIn("Nel\"ly");
        CHECK(signed_in.status == SignInResult::Status::SignedIn && signed_in.token == "t0k3n"
              && signed_in.player.name == "Nelly" && signed_in.player.test);
        api.SetToken(signed_in.token);
        const auto match = api.Challenge("sam", 2);
        CHECK(match.id == 12 && match.state == "accepted" && match.opponent.wfc.size() == 1
              && match.opponent.wfc[0].friend_code == "0868-9934-5920" && match.rules.games == 3 && match.Open()
              && match.winner.empty());
        CHECK(api.Ready(12, 1000000000, "abc").paired);
        api.Report(12, {{3, 1, "mario", "bowser", "the_palace"}});
        try
        {
            api.Accept(12);
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.Status() == 409 && std::string(e.what()) == "The match is accepted already");
        }
        CHECK(!api.CurrentMatch());
        try
        {
            api.Me();
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.SignedOut());
        }
        const auto lists = api.Players();
        CHECK(lists.online.size() == 1 && lists.online[0].id == 8 && lists.online[0].online && !lists.online[0].busy
              && lists.online[0].activity == "match" && lists.online[0].last_online == 1793000000
              && !lists.online[0].is_friend && lists.recent.empty());
        CHECK(lists.friends.size() == 1 && lists.friends[0].id == 9 && lists.friends[0].is_friend
              && !lists.friends[0].online && lists.friends[0].activity.empty() && !lists.friends[0].last_online);
        const auto set = api.ChallengePlayer(8, 2);
        CHECK(set.stage == "go" && set.you_ready && set.opponent_ready && set.opponent_online
              && set.go_at == 1792771200 && set.playing_at == 0 && set.rematch_of == 11 && set.reason.empty());
        api.ReadyToPlay(12, true);
        api.Rematch(12);
        int interval = 0;
        const auto current = api.CurrentMatch(&interval);
        CHECK(current && current->stage == "go" && interval == 2);
        CHECK(api.Me().discord_id == "80351110224678912");
        const auto added = api.AddFriend(std::int64_t(8));
        CHECK(added.size() == 1 && added[0].id == 8 && added[0].is_friend);
        CHECK(api.AddFriend(std::string("b\"o")).size() == 2);
        api.SetActivity("game");
        const auto removed = api.RemoveFriend(8);
        CHECK(removed.size() == 1 && removed[0].id == 9 && removed[0].chat && removed[0].unread == 2);
        const auto page = api.Chat(9, 5);
        CHECK(page.can_send && page.messages.size() == 1 && page.messages[0].id == 6 && !page.messages[0].mine
              && page.messages[0].body == "hi" && page.messages[0].sent_at == 1793000100);
        const auto written = api.SendChat(9, "yo \"x\"");
        CHECK(written.id == 7 && written.mine && written.body == "yo \"x\"");
    }
    CHECK(sent.size() == exchanges.size());
    for (const auto& request : sent) CHECK(request.expected);
    if (sent.size() == exchanges.size())
    {
        CHECK(sent[1].body == R"({"name":"Nel\"ly"})" && sent[1].content_type == "application/json"
              && sent[1].authorization.empty());
        CHECK(sent[2].body == R"({"opponent":"sam","rule_set":2})" && sent[2].authorization == "Bearer t0k3n");
        CHECK(sent[3].body == R"({"profile_id":1000000000,"rules_hash":"abc"})");
        CHECK(sent[4].body == R"({"games":[{"my_score":3,"opponent_score":1,"my_captain":"mario",)"
                              R"("opponent_captain":"bowser","stadium":"the_palace"}]})");
        CHECK(sent[5].body == "{}");
        CHECK(sent[6].body.empty() && sent[6].content_type.empty());
        CHECK(sent[9].body == R"({"opponent_id":8,"rule_set":2})");
        CHECK(sent[10].body == R"({"ready":true})");
        CHECK(sent[11].body == "{}");
        CHECK(sent[14].body == R"({"player_id":8})" && sent[14].content_type == "application/json");
        CHECK(sent[15].body == R"({"name":"b\"o"})");
        CHECK(sent[16].authorization == "Bearer t0k3n" && sent[16].activity == "game" && sent[15].activity.empty());
        CHECK(sent[18].body == R"({"body":"yo \"x\""})");
    }

    // Unencrypted only for addresses ProfileServerAddress lets through.
    HttpRequest request;
    request.url = "http://127.0.0.1:1/";
    try
    {
        HttpGet(request);
        CHECK(false);
    }
    catch (const HttpError& e)
    {
        CHECK(std::string(e.what()).find("Only https") != std::string::npos);
    }
}
#endif
} // namespace

int main()
{
    Addresses();
    Sessions();
    Rules();
    LastOnline();
#ifndef _WIN32
    Requests();
#endif
    if (failures)
    {
        std::fprintf(stderr, "%d profile service checks failed\n", failures);
        return 1;
    }
    std::printf("Profile service checks passed\n");
    return 0;
}
