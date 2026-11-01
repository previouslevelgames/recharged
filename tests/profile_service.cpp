// The profile service client of competitive play: server addresses, the
// saved session, the rules the game module gets, and (where the system has
// libcurl) the requests themselves against a small HTTP server here.
#include "platform/competitive_rules.h"
#include "platform/http_client.h"
#include "platform/online_panel_feed.h"
#include "platform/profile_service.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
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

    // The Discord friends' link, beside the session.
    const auto link = DiscordLinkFile(folder, "/games/recharged/second.ini");
    CHECK(link == folder / "online" / "second.discord");
    CHECK(!LoadDiscordLink(link));
    SaveDiscordLink(link, {"https://wfc.example.com", "refresh \"token\""});
    const auto saved = LoadDiscordLink(link);
    CHECK(saved && saved->server == "https://wfc.example.com" && saved->refresh_token == "refresh \"token\"");
#ifndef _WIN32
    CHECK(::stat(link.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
#endif
    SaveDiscordLink(link, {"https://wfc.example.com", ""});
    CHECK(!LoadDiscordLink(link));
    ForgetProfileSession(link);
    CHECK(!std::filesystem::exists(link));
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
        {"GET /api/v1/server", 200,
         R"({"version":"1.0.4","sign_in":{"discord":true,"test":false},"discord_application":"1561321462327943259"})"},
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
        {"POST /api/v1/discord/token", 200,
         R"({"access_token":"a","refresh_token":"r","expires_in":604800,"scope":"openid sdk.social_layer_presence"})"},
        {"POST /api/v1/discord/token", 400, R"({"error":"Discord refused: connect Discord again"})"},
        {"POST /api/v1/players/discord", 200,
         R"({"players":[{"id":8,"name":"Sam","online":true,"discord_id":"41771983423143937"}]})"},
        // A Discord sign-in that brings the Discord SDK's tokens (1.2.1).
        {"GET /api/v1/sign-in/abc", 200,
         R"({"status":"signed_in","token":"t2","player":{"id":7,"name":"Nelly","discord":"nelly","test":false},)"
         R"("discord":{"access_token":"a2","refresh_token":"r2","expires_in":604800,"scope":"identify openid sdk.social_layer_presence"}})"},
        // The RANKED leaderboards (1.2.1).
        {"GET /api/v1/leaderboard", 200,
         R"({"season":{"first_day":"2026-10-01","last_day":"2026-10-31","players":3,"top":[)"
         R"({"rank":1,"mii":"Mario","player":null,"wins":5,"losses":1,"points":50,"region":2,"you":false},)"
         R"({"rank":2,"mii":"Nelly","player":"Nelly","wins":3,"losses":null,"points":30,"region":4,"you":true}],)"
         R"("you":{"rank":2,"mii":"Nelly","player":"Nelly","wins":3,"losses":null,"points":30,"region":4,"you":true}},)"
         R"("daily":{"first_day":"2026-10-31","last_day":"2026-10-31","players":0,"top":[],"you":null}})"},
        // Available for random games (1.2.1).
        {"PUT /api/v1/me/available", 200, R"({"available":true})"},
        // Mii friends (1.2.1).
        {"POST /api/v1/friends/miis", 200,
         R"({"mii_friends":[{"profile_id":1000000004,"mii":"Tommy","game":"R4QE","friend_code":"3789-5712-2052","online":true,)"
         R"("status":"matchmaking","last_online":1793000000,"friend":true}]})"},
        {"DELETE /api/v1/friends/miis/1000000004", 200, R"({"mii_friends":[]})"},
    };
    std::vector<Sent> sent;
    {
        Server server(exchanges, sent);
        ProfileService api(server.Url());
        const auto info = api.Info();
        CHECK(info.version == "1.0.4" && info.discord && !info.test
              && info.discord_application == "1561321462327943259");
        const auto signed_in = api.TestSignIn("Nel\"ly");
        CHECK(signed_in.status == SignInResult::Status::SignedIn && signed_in.token == "t0k3n"
              && signed_in.player.name == "Nelly" && signed_in.player.test && !signed_in.discord);
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
        const auto tokens = api.DiscordToken("c0de", "v", "http://127.0.0.1:5000/callback");
        CHECK(tokens.access_token == "a" && tokens.refresh_token == "r" && tokens.expires_in == 604800);
        try
        {
            api.RefreshDiscordToken("old");
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.Status() == 400 && !e.SignedOut());
        }
        const auto found = api.PlayersByDiscord({"41771983423143937", "123"});
        CHECK(found.size() == 1 && found[0].id == 8 && found[0].online && found[0].discord_id == "41771983423143937");
        const auto with_discord = api.PollSignIn("abc");
        CHECK(with_discord.status == SignInResult::Status::SignedIn && with_discord.token == "t2" && with_discord.discord
              && with_discord.discord->access_token == "a2" && with_discord.discord->refresh_token == "r2"
              && with_discord.discord->expires_in == 604800);
        const auto board = api.RankedLeaderboard();
        CHECK(board.season.players == 3 && board.season.top.size() == 2 && board.season.first_day == "2026-10-01"
              && board.season.top[0].mii == "Mario" && board.season.top[0].player.empty()
              && board.season.top[0].wins == 5 && board.season.top[1].you && !board.season.top[1].losses
              && board.season.you && board.season.you->rank == 2 && board.season.you->region == 4);
        CHECK(board.daily.players == 0 && board.daily.top.empty() && !board.daily.you);
        CHECK(api.SetAvailable(true));
        const auto miis = api.AddMiiFriend(1000000004);
        CHECK(miis.size() == 1 && miis[0].mii == "Tommy" && miis[0].game == "R4QE" && miis[0].online
              && miis[0].friend_code == "3789-5712-2052"
              && miis[0].status == "matchmaking" && miis[0].last_online == 1793000000 && miis[0].is_friend);
        CHECK(api.RemoveMiiFriend(1000000004).empty());
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
        CHECK(sent[19].body == R"({"code":"c0de","code_verifier":"v","redirect_uri":"http://127.0.0.1:5000/callback"})");
        CHECK(sent[20].body == R"({"refresh_token":"old"})");
        CHECK(sent[21].body == R"({"ids":["41771983423143937","123"]})");
        CHECK(sent[24].body == R"({"available":true})" && sent[24].content_type == "application/json");
        CHECK(sent[25].body == R"({"profile_id":1000000004})" && sent[26].body.empty());
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

// Players without Discord: a Mii sign-in by a profile name (refused while
// the server takes no more), the account it gives, and connecting Discord
// to it later with the session, which a Discord account with a profile of
// its own refuses.
void MiiAccounts()
{
    if (!HttpUnavailable().empty()) return;
    const std::vector<Exchange> exchanges{
        {"GET /api/v1/server", 200, R"({"version":"1.2.4","sign_in":{"discord":true,"test":false,"mii":true}})"},
        {"POST /api/v1/sign-in/mii", 429, R"({"error":"too many new players from here: try again in an hour"})"},
        {"POST /api/v1/sign-in/mii", 400, R"({"error":"a name has 1 to 32 characters, without control characters"})"},
        {"POST /api/v1/sign-in/mii", 200,
         R"({"status":"signed_in","token":"m0","player":{"id":10,"name":"Al","account":"mii","friend_code":null,)"
         R"("discord_id":null,"wfc":[]}})"},
        {"POST /api/v1/sign-in/mii", 200,
         R"({"token":"m1","player":{"id":11,"name":"Bob the Builder","account":"mii","wfc":[]}})"},
        {"GET /api/v1/me", 200,
         R"({"id":11,"name":"Bob the Builder","account":"mii","friend_code":"0868-9934-5920",)"
         R"("wfc":[{"profile_id":1000000000,"game":"R4QE","friend_code":"0868-9934-5920","mii":"Bob"}]})"},
        {"POST /api/v1/sign-in", 200,
         R"({"id":"up-1","code":"K7Q2","url":"http://127.0.0.1/sign-in/up-1","expires_in":600,"poll_interval":2,)"
         R"("link":true})"},
        {"GET /api/v1/sign-in/up-1", 409, R"({"error":"this Discord account already has a Recharged profile"})"},
        {"GET /api/v1/sign-in/up-1", 200, R"({"status":"refused","message":"Discord said no"})"},
        {"GET /api/v1/sign-in/up-1", 200,
         R"({"status":"signed_in","token":"m2","player":{"id":11,"name":"Bob","discord":"bob","account":"discord"}})"},
        {"GET /api/v1/me", 200, R"({"id":7,"name":"Nelly","discord":"nelly","test":false})"},
    };
    std::vector<Sent> sent;
    {
        Server server(exchanges, sent);
        ProfileService api(server.Url());
        const auto info = api.Info();
        CHECK(info.mii && info.discord && !info.test);
        try
        {
            api.MiiSignIn("Bob the Builder");
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.Status() == 429 && !e.SignedOut()
                  && std::string(e.what()) == "Too many new players from here: try again in an hour");
        }
        try
        {
            api.MiiSignIn("x\x01");
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.Status() == 400);
        }
        const auto first = api.MiiSignIn("Al");
        CHECK(first.status == SignInResult::Status::SignedIn && first.token == "m0" && first.player.MiiAccount()
              && first.player.friend_code.empty() && first.player.discord_id.empty());
        const auto signed_in = api.MiiSignIn("Bob the Builder");
        CHECK(signed_in.status == SignInResult::Status::SignedIn && signed_in.token == "m1"
              && signed_in.player.id == 11 && signed_in.player.MiiAccount() && !signed_in.player.test
              && signed_in.player.discord.empty() && !signed_in.discord);
        api.SetToken(signed_in.token);
        const auto me = api.Me();
        CHECK(me.MiiAccount() && me.friend_code == "0868-9934-5920" && me.wfc.size() == 1 && me.wfc[0].mii == "Bob");
        // Connecting Discord: the sign-in with the Mii player's session.
        const auto start = api.StartSignIn();
        CHECK(start.id == "up-1" && start.code == "K7Q2" && start.link);
        const auto taken = api.PollSignIn(start.id);
        CHECK(taken.status == SignInResult::Status::Refused
              && taken.message == "This Discord account already has a Recharged profile");
        const auto refused = api.PollSignIn(start.id);
        CHECK(refused.status == SignInResult::Status::Refused && refused.message == "Discord said no");
        const auto connected = api.PollSignIn(start.id);
        CHECK(connected.status == SignInResult::Status::SignedIn && connected.token == "m2"
              && connected.player.account == "discord" && !connected.player.MiiAccount());
        // Servers before Mii accounts say nothing of the account.
        const auto older = api.Me();
        CHECK(older.account.empty() && !older.MiiAccount() && older.friend_code.empty());
    }
    CHECK(sent.size() == exchanges.size());
    for (const auto& request : sent) CHECK(request.expected);
    if (sent.size() == exchanges.size())
    {
        CHECK(sent[1].body == R"({"name":"Bob the Builder"})" && sent[1].content_type == "application/json"
              && sent[1].authorization.empty());
        CHECK(sent[2].body == R"({"name":"x\u0001"})");
        CHECK(sent[4].authorization.empty() && sent[5].authorization == "Bearer m1");
        CHECK(sent[6].body == "{}" && sent[6].authorization == "Bearer m1");
        CHECK(sent[7].authorization == "Bearer m1");
    }
}

// The game's online panel with the Discord friends the launcher gave it: they
// are looked up with the lists, in parts as large as the server takes; a
// failed lookup keeps the last list, fails nothing else and is logged once.
void FeedDiscordFriends()
{
    if (!HttpUnavailable().empty()) return;
    using namespace std::chrono_literals;
    std::string off;
    CHECK(!OnlinePanelFeed::Start("", "unused.session", "", {"1"}, {}, &off) && !off.empty());
    const auto folder = std::filesystem::temp_directory_path()
        / ("recharged-feed-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(folder);
    const auto session = ProfileSessionFile(folder, "feed.ini");
    SaveProfileSession(session, {"http://127.0.0.1:1", "t0k3n", "Nelly"});
    off.clear();
    CHECK(!OnlinePanelFeed::Start("http://127.0.0.1:2", session, "", {"1"}, {}, &off) && !off.empty());

    std::vector<std::string> ids;
    for (int i = 0; i < 250; ++i) ids.push_back(std::to_string(900000000000000000LL + i));
    const std::string me = R"({"id":7,"name":"Nelly","discord":"nelly","discord_id":"80351110224678912"})";
    const std::string lists = R"({"online":[],"recent":[],"friends":[{"id":9,"name":"Zed","online":false,"friend":true}]})";
    const std::string none = R"({"match":null})";
    const std::vector<Exchange> exchanges{
        {"GET /api/v1/me", 200, me},
        {"GET /api/v1/players", 200, lists},
        {"POST /api/v1/players/discord", 200,
         R"({"players":[{"id":9,"name":"Zed","online":false,"discord_id":"900000000000000003"}]})"},
        {"POST /api/v1/players/discord", 200,
         R"({"players":[{"id":8,"name":"Sam","online":true,"busy":false,"activity":"game",)"
         R"("discord_id":"900000000000000210"}]})"},
        {"GET /api/v1/matches/current", 200, none},
        {"GET /api/v1/rules", 200,
         R"({"rule_sets":[{"id":2,"name":"Competitive","rules":{"tweaks":[],"skill":"rookie","limit":"time",)"
         R"("time":180,"goals":5,"games":3,"stadium":"random"},"hash":"abc"}]})"},
        {"GET /api/v1/server", 200, R"({"version":"1.2.1","sign_in":{"discord":true,"test":false}})"},
        // The next two asks: the lookup fails, the lists don't.
        {"GET /api/v1/me", 200, me},
        {"GET /api/v1/players", 200, lists},
        {"POST /api/v1/players/discord", 500, R"({"error":"down for a moment"})"},
        {"GET /api/v1/matches/current", 200, none},
        {"GET /api/v1/me", 200, me},
        {"GET /api/v1/players", 200, lists},
        {"POST /api/v1/players/discord", 500, R"({"error":"still down"})"},
        {"GET /api/v1/matches/current", 200, none},
    };
    std::vector<Sent> sent;
    std::mutex log_mutex;
    std::vector<std::string> logs;
    OnlinePanelFeed::Snapshot first, last;
    {
        Server server(exchanges, sent);
        SaveProfileSession(session, {server.Url(), "t0k3n", "Nelly"});
        auto feed = OnlinePanelFeed::Start(
            server.Url(), session, "", ids,
            [&](const std::string& text) {
                std::lock_guard<std::mutex> lock(log_mutex);
                logs.push_back(text);
            },
            &off);
        CHECK(feed != nullptr);
        if (feed)
        {
            feed->SetOpen(true);
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            while (std::chrono::steady_clock::now() < deadline)
            {
                last = feed->Latest();
                if (last.version == 1 && !first.loaded) first = last;
                if (last.version >= 3) break;
                std::this_thread::sleep_for(20ms);
            }
            feed.reset();
        }
    }
    std::filesystem::remove_all(folder);

    // The first answer: both parts, online first.
    CHECK(first.loaded && first.error.empty() && first.discord_friends.size() == 2);
    if (first.discord_friends.size() == 2)
    {
        CHECK(first.discord_friends[0].id == 8 && first.discord_friends[0].online
              && first.discord_friends[0].activity == "game"
              && first.discord_friends[0].discord_id == "900000000000000210");
        CHECK(first.discord_friends[1].id == 9 && !first.discord_friends[1].online);
    }
    CHECK(first.lists.friends.size() == 1 && first.server_version == "1.2.1" && first.rule_sets.size() == 1);
    // The failed lookups keep that list, and the lists still come.
    CHECK(last.version == 3 && last.error.empty() && last.lists.friends.size() == 1);
    CHECK(last.discord_friends.size() == 2 && last.discord_friends[0].id == 8);
    CHECK(logs.size() == 1 && logs[0].find("Down for a moment") != std::string::npos);

    CHECK(sent.size() == exchanges.size());
    for (const auto& request : sent) CHECK(request.expected);
    if (sent.size() == exchanges.size())
    {
        auto body = [&](std::size_t from, std::size_t to) {
            std::string text = "{\"ids\":[";
            for (std::size_t i = from; i < to; ++i) text += (i > from ? ",\"" : "\"") + ids[i] + "\"";
            return text + "]}";
        };
        CHECK(sent[2].body == body(0, 200) && sent[3].body == body(200, 250));
        CHECK(sent[2].authorization == "Bearer t0k3n" && sent[2].activity == "game"
              && sent[2].content_type == "application/json");
    }
}

// The panel's choice of rules: each challenge goes out with the rule set
// chosen when it was asked for (by name; one the server doesn't have: its first).
void FeedChallengeRules()
{
    if (!HttpUnavailable().empty()) return;
    using namespace std::chrono_literals;
    const auto folder = std::filesystem::temp_directory_path()
        / ("recharged-feed-rules-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(folder);
    const auto session = ProfileSessionFile(folder, "feed.ini");
    const std::string me = R"({"id":7,"name":"Nelly","discord":"nelly"})";
    const std::string lists = R"({"online":[],"recent":[],"friends":[]})";
    const std::string none = R"({"match":null})";
    const std::vector<Exchange> exchanges{
        {"GET /api/v1/me", 200, me},
        {"GET /api/v1/players", 200, lists},
        {"GET /api/v1/matches/current", 200, none},
        {"GET /api/v1/rules", 200,
         R"({"rule_sets":[{"id":2,"name":"Competitive","rules":{"tweaks":[],"skill":"rookie","limit":"time",)"
         R"("time":180,"goals":5,"games":3,"stadium":"random"},"hash":"abc"},)"
         R"({"id":5,"name":"Classic","rules":{"tweaks":[],"skill":"legend","limit":"time",)"
         R"("time":300,"goals":5,"games":1,"stadium":"random"},"hash":"def"}]})"},
        {"GET /api/v1/server", 200, R"({"version":"1.2.1","sign_in":{"discord":true,"test":false}})"},
        // Classic, chosen in the panel; then a name the server doesn't have.
        {"POST /api/v1/matches", 200, kSet},
        {"GET /api/v1/me", 200, me},
        {"GET /api/v1/players", 200, lists},
        {"GET /api/v1/matches/current", 200, none},
        {"POST /api/v1/matches", 200, kSet},
        {"GET /api/v1/me", 200, me},
        {"GET /api/v1/players", 200, lists},
        {"GET /api/v1/matches/current", 200, none},
    };
    std::vector<Sent> sent;
    std::string off;
    {
        Server server(exchanges, sent);
        SaveProfileSession(session, {server.Url(), "t0k3n", "Nelly"});
        auto feed = OnlinePanelFeed::Start(server.Url(), session, "Competitive", {}, {}, &off);
        CHECK(feed != nullptr);
        if (feed)
        {
            CHECK(feed->ChallengeRules() == "Competitive");
            const auto wait = [&](std::uint64_t version) {
                const auto deadline = std::chrono::steady_clock::now() + 30s;
                while (std::chrono::steady_clock::now() < deadline && feed->Latest().version < version)
                    std::this_thread::sleep_for(10ms);
                return feed->Latest().version >= version;
            };
            feed->SetOpen(true);
            CHECK(wait(1) && feed->Latest().rule_sets.size() == 2);
            feed->SetOpen(false); // only the actions ask now
            feed->SetChallengeRules("Classic");
            CHECK(feed->ChallengeRules() == "Classic");
            feed->Challenge(8);
            feed->SetChallengeRules("Gone"); // after the ask: the challenge keeps Classic
            CHECK(wait(2));
            feed->Challenge(9);
            CHECK(wait(3));
            feed.reset();
        }
    }
    std::filesystem::remove_all(folder);
    CHECK(sent.size() == exchanges.size());
    for (const auto& request : sent) CHECK(request.expected);
    if (sent.size() == exchanges.size())
    {
        CHECK(sent[5].body == R"({"opponent_id":8,"rule_set":5})");
        CHECK(sent[9].body == R"({"opponent_id":9,"rule_set":2})");
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
    MiiAccounts();
    FeedDiscordFriends();
    FeedChallengeRules();
#endif
    if (failures)
    {
        std::fprintf(stderr, "%d profile service checks failed\n", failures);
        return 1;
    }
    std::printf("Profile service checks passed\n");
    return 0;
}
