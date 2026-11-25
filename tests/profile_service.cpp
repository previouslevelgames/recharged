// The profile service client of competitive play: server addresses, the
// saved session, the rules the game module gets, and (where the system has
// libcurl) the requests themselves against a small HTTP server here.
#include "platform/competitive_rules.h"
#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/online_panel_feed.h"
#include "platform/profile_service.h"
#include "platform/tournament_feed.h"

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

    // The profiles that signed in here, beside the session, kept in order.
    const auto known = KnownProfilesFile(folder, "/games/recharged/second.ini");
    CHECK(known == folder / "online" / "second.profiles");
    CHECK(LoadKnownProfiles(known).empty());
    SaveKnownProfiles(known, {{"https://wfc.example.com", 11, "Bob \"the\" Builder", "mii", {"0868-9934-5920"}},
                              {"https://wfc.example.com", 12, "Anosch", "discord", {"0310-6477-1073", "1234-5678-9012"}},
                              {"https://other.example.com", 13, "Sam", "test", {}}});
    const auto profiles = LoadKnownProfiles(known);
    CHECK(profiles.size() == 3);
    if (profiles.size() == 3)
    {
        CHECK(profiles[0].server == "https://wfc.example.com" && profiles[0].player_id == 11
              && profiles[0].name == "Bob \"the\" Builder" && profiles[0].account == "mii"
              && profiles[0].friend_codes == std::vector<std::string>{"0868-9934-5920"});
        CHECK(profiles[1].account == "discord" && profiles[1].friend_codes.size() == 2
              && profiles[1].friend_codes[1] == "1234-5678-9012");
        CHECK(profiles[2].name == "Sam" && profiles[2].friend_codes.empty());
    }
    { std::ofstream(known) << "{\"profiles\":[{\"server\":\"\",\"id\":5},{\"server\":\"x\",\"id\":0}]}"; }
    CHECK(LoadKnownProfiles(known).empty());
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
    CHECK(module->stadium_cheat == 0 && module->player_cheat == 0 && module->powerup_cheat == 0);
    CHECK(DescribeRules(rules)
          == "First to 3 goals, best of 5, The Palace, Fast terrain, No Mega Strikes, Thunder Island walls");
    // The cheats: their enum values for the module, the game's names.
    rules.stadium_cheat = "field_tilt";
    rules.player_cheat = "classic_mode";
    rules.powerup_cheat = "no_power_ups";
    const auto cheats = ModuleRules(rules, opponent);
    CHECK(cheats && cheats->stadium_cheat == 4 && cheats->player_cheat == 4 && cheats->powerup_cheat == 11);
    CHECK(DescribeRules(rules) == "First to 3 goals, best of 5, The Palace, Fast terrain, No Mega Strikes, Thunder "
                                  "Island walls, Field Tilt, Classic Mode, No Power-Ups");
    CHECK(RulesJson(rules)
          == R"({"tweaks":["fast_terrain","no_mega_strikes","thunder_island_walls"],"skill":"superstar","limit":"goals",)"
             R"("time":180,"goals":3,"games":5,"stadium":"the_palace","stadium_cheat":"field_tilt",)"
             R"("player_cheat":"classic_mode","powerup_cheat":"no_power_ups"})");
    rules.powerup_cheat = "butterfingers";
    CHECK(!ModuleRules(rules, opponent));
    rules.stadium_cheat = rules.player_cheat = rules.powerup_cheat = "none";
    CHECK(StadiumCheatChoices().size() == 6 && PlayerCheatChoices().size() == 5 && PowerupCheatChoices().size() == 12
          && StadiumChoices().size() == 17 && std::string(StadiumChoices()[11].label) == "The Palace"
          && RuleTweakChoices().size() == 4);
    rules.limit = "time";
    rules.time = 150;
    rules.games = 1;
    rules.stadium = "random";
    rules.tweaks.clear();
    CHECK(DescribeRules(rules) == "2:30 minutes, one game, random stadium");
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
    // Profile names as the game's screens show them.
    CHECK(ScreenName("Christopher Columbus the Great") == u"Christopher Columbus the Great");
    CHECK(ScreenName("  Bob \t the\u00a0 Builder  ") == u"Bob the Builder");
    CHECK(ScreenName("Zo\u00eb \U0001F600 \u200bK\u00e4the\uE000") == u"Zo\u00eb K\u00e4the");
    CHECK(ScreenName("\U0001F600\U0001F680").empty() && ScreenName("bad \xff byte").empty());
    CHECK(ScreenName(std::string(40, 'x')) == std::u16string(32, u'x'));
    CHECK(ScreenName("\xe3\x83\x9e\xe3\x83\xaa\xe3\x82\xaa") == u"\u30DE\u30EA\u30AA");
    // Malformed UTF-8 is refused: an overlong form, a surrogate, a value past
    // U+10FFFF, a missing continuation byte.
    CHECK(ScreenName("a\xc0\xaf").empty() && ScreenName("a\xed\xa0\x80").empty()
          && ScreenName("a\xf4\x90\x80\x80").empty() && ScreenName("a\xc3" "b").empty());
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
    std::string body, authorization, content_type, activity, match_kind, game, screen, set_game;
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
            sent.match_kind = header("Recharged-Match: ");
            sent.game = header("Recharged-Game: ");
            sent.screen = header("Recharged-Screen: ");
            sent.set_game = header("Recharged-Set-Game: ");
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
         R"("activity":"match","match":"cpu","last_online":1793000000,"friend":false,"screen":"cup_team",)"
         R"("searching":true,"set":{"stage":"playing","challenger":true,"opponent":"Bo","games":3,"game":2}}],)"
         R"("recent":[],)"
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
        MatchGame broke_off{1, 0, "mario", "bowser", "vice"};
        broke_off.interrupted = true;
        broke_off.played = 0.4f;
        api.Report(12, {{3, 1, "mario", "bowser", "the_palace"}, broke_off});
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
              && lists.online[0].activity == "match" && lists.online[0].match_kind == "cpu"
              && lists.online[0].last_online == 1793000000
              && !lists.online[0].is_friend && lists.recent.empty());
        CHECK(lists.friends.size() == 1 && lists.friends[0].id == 9 && lists.friends[0].is_friend
              && !lists.friends[0].online && lists.friends[0].activity.empty() && !lists.friends[0].last_online);
        // What their game shows, their search and their set (servers from 1.2.12 on).
        CHECK(lists.online[0].screen == "cup_team" && lists.online[0].searching && lists.online[0].set
              && lists.online[0].set->stage == "playing" && lists.online[0].set->challenger
              && lists.online[0].set->opponent == "Bo" && lists.online[0].set->games == 3
              && lists.online[0].set->game == 2);
        CHECK(lists.friends[0].screen.empty() && !lists.friends[0].searching && !lists.friends[0].set);
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
        api.SetActivity("game", {}, "online");
        const auto removed = api.RemoveFriend(8);
        CHECK(removed.size() == 1 && removed[0].id == 9 && removed[0].chat && removed[0].unread == 2);
        api.SetActivity("match", "ranked", {}, 2);
        ProfileService::SetPlayedGame("R4QP01"); // the game's id: its first four characters go
        const auto page = api.Chat(9, 5);
        ProfileService::SetPlayedGame({});
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
                              R"("opponent_captain":"bowser","stadium":"the_palace"},{"my_score":1,"opponent_score":0,)"
                              R"("my_captain":"mario","opponent_captain":"bowser","stadium":"vice",)"
                              R"("interrupted":true,"played":0.400}]})");
        CHECK(sent[5].body == "{}");
        CHECK(sent[6].body.empty() && sent[6].content_type.empty());
        CHECK(sent[9].body == R"({"opponent_id":8,"rule_set":2})");
        CHECK(sent[10].body == R"({"ready":true})");
        CHECK(sent[11].body == "{}");
        CHECK(sent[14].body == R"({"player_id":8})" && sent[14].content_type == "application/json");
        CHECK(sent[15].body == R"({"name":"b\"o"})");
        CHECK(sent[16].authorization == "Bearer t0k3n" && sent[16].activity == "game" && sent[15].activity.empty());
        CHECK(sent[16].match_kind.empty() && sent[17].activity == "match" && sent[17].match_kind == "ranked");
        CHECK(sent[16].game.empty() && sent[17].game == "R4QP" && sent[18].game.empty());
        CHECK(sent[16].screen == "online" && sent[16].set_game.empty() && sent[15].screen.empty());
        CHECK(sent[17].screen.empty() && sent[17].set_game == "2");
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

// Signing in again through the game: its WFC login proof, without a session
// and as the game; a WFC profile that identifies nobody is refused (404).
void WfcSignIn()
{
    if (!HttpUnavailable().empty()) return;
    const std::vector<Exchange> exchanges{
        {"POST /api/v1/sign-in/wfc", 404, R"({"error":"no Recharged profile signs in with this WFC profile"})"},
        {"POST /api/v1/sign-in/wfc", 200,
         R"({"status":"signed_in","token":"w1","player":{"id":11,"name":"Bob the Builder","account":"mii",)"
         R"("friend_code":"0868-9934-5920"}})"},
    };
    std::vector<Sent> sent;
    {
        Server server(exchanges, sent);
        ProfileService api(server.Url());
        api.SetActivity("game");
        try
        {
            api.WfcSignIn(1000000001u, "lt-other");
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.Status() == 404);
        }
        const auto back = api.WfcSignIn(1000000000u, "lt-\"x\"");
        CHECK(back.status == SignInResult::Status::SignedIn && back.token == "w1" && back.player.id == 11
              && back.player.MiiAccount() && back.player.friend_code == "0868-9934-5920");
    }
    CHECK(sent.size() == exchanges.size());
    for (const auto& request : sent) CHECK(request.expected);
    if (sent.size() == exchanges.size())
    {
        CHECK(sent[0].body == R"({"profile_id":1000000001,"login_ticket":"lt-other"})");
        CHECK(sent[1].body == R"({"profile_id":1000000000,"login_ticket":"lt-\"x\""})"
              && sent[1].authorization.empty() && sent[1].activity == "game"
              && sent[1].content_type == "application/json");
    }
}

// Signed out: which of the game save's Miis play for a profile already.
void KnownWfc()
{
    if (!HttpUnavailable().empty()) return;
    const std::vector<Exchange> exchanges{
        {"POST /api/v1/sign-in/wfc/known", 200,
         R"({"profiles":[{"profile_id":1000000000,"sign_in":"game"},{"profile_id":1000000002,"sign_in":"discord"},)"
         R"({"profile_id":-4,"sign_in":"game"}]})"},
        {"POST /api/v1/sign-in/wfc/known", 404, R"({"error":"not found"})"},
    };
    std::vector<Sent> sent;
    {
        Server server(exchanges, sent);
        ProfileService api(server.Url());
        const auto known = api.KnownWfc({1000000000u, 1000000001u, 1000000002u});
        CHECK(known.size() == 2);
        if (known.size() == 2)
            CHECK(known[0].first == 1000000000u && known[0].second == "game" && known[1].first == 1000000002u
                  && known[1].second == "discord");
        try
        {
            api.KnownWfc({7u});
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.Status() == 404);
        }
    }
    CHECK(sent.size() == exchanges.size());
    if (sent.size() == exchanges.size())
    {
        CHECK(sent[0].body == R"({"profile_ids":[1000000000,1000000001,1000000002]})"
              && sent[0].authorization.empty());
        CHECK(sent[1].body == R"({"profile_ids":[7]})");
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
        // By the server's Classic as it is; then by rules of the player's own.
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
            feed->Challenge(8, 5, {}, "Classic");
            CHECK(wait(2));
            MatchRules own = feed->Latest().rule_sets[0].rules;
            own.stadium_cheat = "field_tilt";
            feed->Challenge(9, 0, own, "Competitive (changed)");
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
        CHECK(sent[9].body
              == R"({"opponent_id":9,"rules":{"tweaks":[],"skill":"rookie","limit":"time","time":180,"goals":5,)"
                 R"("games":3,"stadium":"random","stadium_cheat":"field_tilt","player_cheat":"none",)"
                 R"x("powerup_cheat":"none"},"name":"Competitive (changed)"})x");
    }
}
#endif
} // namespace

// Tournaments: the list, a page with its bracket and the player's place,
// joining, READY with its match, and a player without Discord refused.
void Tournaments()
{
    if (!HttpUnavailable().empty()) return;
    const std::string tournament = R"({"id":4,"name":"Recharged Cup","organizer":null,"format":"double",)"
                                   R"("state":"running","rule_set":"Competitive","rules":{"tweaks":[],"skill":"rookie",)"
                                   R"("limit":"goals","time":180,"goals":10,"games":3,"stadium":"random"},"best_of":3,)"
                                   R"("semifinals_best_of":5,"finals_best_of":5,"max_entrants":null,"entrants":2,"starts_at":null,)"
                                   R"("created_at":1,"started_at":2,"ended_at":null})";
    const std::string page = R"({"tournament":)" + tournament
        + R"(,"entrants":[{"player":{"id":7,"name":"Mario","account":"discord"},"seed":0,"dropped":false,)"
          R"("place":null},{"player":{"id":8,"name":"Bowser","account":"discord"},"seed":1,"dropped":false,)"
          R"("place":null}],"sets":[{"id":30,"index":0,"side":"winners","round":1,"position":0,"games":5,)"
          R"("state":"ready","home":7,"away":8,"winner":null,"decided_by":null,"home_wins":null,)"
          R"("from":[{"seed":0},{"seed":1}],)"
          R"("away_wins":null,"match":null,"game_home":8,"played":[{"home_captain":"mario","away_captain":)"
          R"("bowser","home_score":3,"away_score":10,"stadium":"the_palace","interrupted":false}]},{"id":31,"index":1,"side":"grand_final","round":1,"position":0,)"
          R"("games":5,"state":"pending","home":null,"away":null,"winner":null,"decided_by":null,)"
          R"("from":[{"winner":30},{"loser":30}],)"
          R"("home_wins":null,"away_wins":null,"match":null}],"me":{"player":7,"entrant":true,"eligible":true,)"
          R"("reason":null,"dropped":false,"place":null,"set":30},"poll_interval":5})";
    const std::string made = R"({"match":{"id":12,"state":"accepted","side":"challenger","opponent":{"id":8,)"
                             R"("name":"Bowser"},"rule_set":"Competitive","rules":{"tweaks":[],"skill":"rookie",)"
                             R"("limit":"goals","time":180,"goals":10,"games":5,"stadium":"random"},)"
                             R"("rules_hash":"h","stage":"open","you_ready":true,"opponent_ready":false,)"
                             R"("tournament":{"id":4,"name":"Recharged Cup","set":30,"side":"winners","round":1,)"
                             R"("games":5,"home_wins":0,"away_wins":1,"won":0,"lost":1,"state":"ready",)"
                             R"("winner":null,"home":"you"}}})";
    const std::vector<Exchange> exchanges{
        {"GET /api/v1/tournaments", 200, R"({"tournaments":[)" + tournament.substr(0, tournament.size() - 1)
                                             + R"(,"joined":true}]})"},
        {"GET /api/v1/tournaments/4", 200, page},
        {"POST /api/v1/tournaments/4/join", 403, R"({"error":"discord_required","message":"connect Discord"})"},
        {"POST /api/v1/tournaments/4/sets/30/ready", 200, made},
        {"POST /api/v1/tournaments/4/sets/30/ready", 200, R"({"match":null})"},
        {"DELETE /api/v1/tournaments/4/join", 200, page},
    };
    std::vector<Sent> sent;
    {
        Server server(exchanges, sent);
        ProfileService api(server.Url(), "t1");
        const auto listed = api.Tournaments();
        CHECK(listed.size() == 1 && listed[0].id == 4 && listed[0].name == "Recharged Cup"
              && listed[0].format == "double" && listed[0].state == "running" && listed[0].joined
              && listed[0].best_of == 3 && listed[0].semifinals_best_of == 5 && listed[0].finals_best_of == 5 && listed[0].max_entrants == 0
              && listed[0].entrants == 2 && listed[0].rules.goals == 10 && listed[0].started_at == 2
              && listed[0].ended_at == 0);
        const auto shown = api.Tournament(4);
        CHECK(shown.entrants.size() == 2 && shown.entrants[1].player.name == "Bowser" && shown.entrants[1].seed == 1
              && shown.entrants[0].place == 0);
        CHECK(shown.sets.size() == 2 && shown.sets[0].state == "ready" && shown.sets[0].home == 7
              && shown.sets[0].away == 8 && shown.sets[0].games == 5 && shown.sets[0].home_wins == -1
              && shown.sets[0].match == 0 && shown.sets[1].home == 0 && shown.sets[1].side == "grand_final"
              && shown.sets[0].game_home == 8 && shown.sets[1].game_home == 0);
        CHECK(shown.sets[0].played.size() == 1 && shown.sets[0].played[0].home_captain == "mario"
              && shown.sets[0].played[0].away_captain == "bowser" && shown.sets[0].played[0].home_score == 3
              && shown.sets[0].played[0].away_score == 10 && !shown.sets[0].played[0].interrupted
              && shown.sets[1].played.empty());
        using Kind = TournamentFrom::Kind;
        CHECK(shown.sets[0].from[1].kind == Kind::Seed && shown.sets[0].from[1].value == 1
              && shown.sets[1].from[0].kind == Kind::Winner && shown.sets[1].from[1].kind == Kind::Loser
              && shown.sets[1].from[1].value == 30);
        CHECK(shown.me && shown.me->entrant && shown.me->eligible && shown.me->set == 30 && shown.me->reason.empty());
        CHECK(shown.Set(30) == &shown.sets[0] && !shown.Set(99) && shown.Entrant(8) == &shown.entrants[1]);
        CHECK(shown.poll_interval == 5);
        try
        {
            api.JoinTournament(4);
            CHECK(false);
        }
        catch (const ProfileError& e)
        {
            CHECK(e.Status() == 403);
        }
        const auto match = api.TournamentReady(4, 30, true);
        CHECK(match && match->id == 12 && match->stage == "open" && match->rules.games == 5
              && match->tournament.id == 4 && match->tournament.set == 30 && match->tournament.name == "Recharged Cup"
              && match->tournament.side == "winners" && match->tournament.round == 1 && match->tournament.games == 5
              && match->tournament.won == 0 && match->tournament.lost == 1 && match->tournament.state == "ready"
              && match->tournament.winner.empty() && match->tournament.home == "you");
        CHECK(!api.TournamentReady(4, 30, false));
        CHECK(api.LeaveTournament(4).tournament.id == 4);
    }
    CHECK(sent.size() == exchanges.size());
    for (const auto& request : sent) CHECK(request.expected);
    if (sent.size() == exchanges.size())
    {
        CHECK(sent[0].authorization == "Bearer t1");
        CHECK(sent[3].body == R"({"ready":true})" && sent[4].body == R"({"ready":false})");
        CHECK(sent[5].body.empty());
    }
    // A set's match from a server without tournaments has none.
    CHECK(ParseJson(kSet)["tournament"].IsNull());
}

// The tournament the page shows, and the feed asking for it and acting on it.
void TournamentFeedRounds()
{
    auto tournament = [](std::int64_t id, const char* state, bool joined) {
        ProfileTournament t;
        t.id = id;
        t.state = state;
        t.joined = joined;
        return t;
    };
    CHECK(!ChooseTournament({}));
    CHECK(ChooseTournament({tournament(1, "finished", true), tournament(2, "running", false)}) == 1u);
    CHECK(ChooseTournament({tournament(1, "registration", false), tournament(2, "running", false)}) == 0u);
    CHECK(ChooseTournament({tournament(1, "registration", false), tournament(2, "registration", true)}) == 1u);
    CHECK(ChooseTournament({tournament(1, "registration", true), tournament(2, "running", true)}) == 1u);
    CHECK(ChooseTournament({tournament(1, "cancelled", false), tournament(2, "finished", false)}) == 1u);
    CHECK(!ChooseTournament({tournament(1, "cancelled", true)}));
    if (!HttpUnavailable().empty()) return;

    const std::string list = R"({"tournaments":[{"id":4,"name":"Cup","format":"single","state":"running",)"
                             R"("joined":true,"best_of":3,"finals_best_of":3,"entrants":2}]})";
    const std::string page = R"({"tournament":{"id":4,"name":"Cup","format":"single","state":"running",)"
                             R"("best_of":3,"finals_best_of":3,"entrants":2},"entrants":[],"sets":[{"id":30,)"
                             R"("state":"STATE","home":7,"away":8,"games":3,"match":MATCH}],"me":{"player":7,)"
                             R"("entrant":true,"eligible":true,"set":30},"poll_interval":60})";
    auto with = [](std::string text, const std::string& state, const std::string& match) {
        text.replace(text.find("STATE"), 5, state);
        text.replace(text.find("MATCH"), 5, match);
        return text;
    };
    const std::string two = R"({"tournaments":[{"id":4,"name":"Cup","format":"single","state":"running",)"
                            R"("joined":true,"best_of":3,"finals_best_of":3,"entrants":2},{"id":5,"name":"Old",)"
                            R"("format":"double","state":"finished","joined":true,"place":2,"best_of":3,)"
                            R"("finals_best_of":3,"entrants":4}]})";
    const std::string other = R"({"tournament":{"id":5,"name":"Old","format":"double","state":"finished",)"
                              R"("best_of":3,"finals_best_of":3,"entrants":4},"entrants":[],"sets":[],)"
                              R"("me":{"player":7,"entrant":true,"eligible":true,"place":2},"poll_interval":60})";
    // Cup called off, and another open for entries: the pages stay on Cup.
    const std::string called_off =
        R"({"tournaments":[{"id":6,"name":"Next","format":"single","state":"registration","joined":false,)"
        R"("best_of":3,"finals_best_of":3,"entrants":1},{"id":4,"name":"Cup","format":"single",)"
        R"("state":"cancelled","joined":true,"best_of":3,"finals_best_of":3,"entrants":2}]})";
    const std::string called_off_page =
        R"({"tournament":{"id":4,"name":"Cup","format":"single","state":"cancelled","best_of":3,)"
        R"("finals_best_of":3,"entrants":2},"entrants":[],"sets":[],"me":{"player":7,"entrant":true,)"
        R"("eligible":true},"poll_interval":60})";
    const std::string match = R"({"id":12,"state":"accepted","stage":"open","you_ready":true,)"
                              R"("opponent_ready":false,"opponent":{"id":8,"name":"Bowser"}})";
    const std::string taken_back = R"({"id":12,"state":"accepted","stage":"open","you_ready":false,)"
                                   R"("opponent_ready":false,"opponent":{"id":8,"name":"Bowser"}})";
    const std::vector<Exchange> exchanges{
        {"GET /api/v1/tournaments", 200, list},
        {"GET /api/v1/tournaments/4", 200, with(page, "ready", "null")},
        {"POST /api/v1/tournaments/4/sets/30/ready", 200, R"({"match":)" + match + "}"},
        {"GET /api/v1/tournaments", 200, list},
        {"GET /api/v1/tournaments/4", 200, with(page, "playing", "12")},
        {"GET /api/v1/matches/current", 200, R"({"match":)" + match + R"(,"poll_interval":2})"},
        // The pages show another tournament (Show): the player's own, of
        // their READY, is asked too.
        {"GET /api/v1/tournaments", 200, two},
        {"GET /api/v1/tournaments/5", 200, other},
        {"GET /api/v1/tournaments/4", 200, with(page, "playing", "12")},
        {"GET /api/v1/matches/current", 200, R"({"match":)" + match + R"(,"poll_interval":2})"},
        // READY taken back there goes to the own one; the page shown that
        // fails to come stays as it was.
        {"POST /api/v1/tournaments/4/sets/30/ready", 200, R"({"match":)" + taken_back + "}"},
        {"GET /api/v1/tournaments", 200, two},
        {"GET /api/v1/tournaments/5", 500, R"({"error":"down"})"},
        {"GET /api/v1/tournaments/4", 200, with(page, "playing", "12")},
        {"GET /api/v1/matches/current", 200, R"({"match":)" + taken_back + R"(,"poll_interval":2})"},
        // Back on the feed's choice (Cup, chosen when the pages opened), Cup
        // called off: it stays, not the one open for entries.
        {"GET /api/v1/tournaments", 200, called_off},
        {"GET /api/v1/tournaments/4", 200, called_off_page},
    };
    const auto folder = std::filesystem::temp_directory_path()
        / ("recharged-feed-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<Sent> sent;
    {
        Server server(exchanges, sent);
        const auto file = folder / "online" / "game.session";
        SaveProfileSession(file, {server.Url(), "t9", "Mario"});
        auto feed = TournamentFeed::Start(server.Url(), file, nullptr, nullptr);
        CHECK(feed != nullptr);
        auto wait = [&](auto&& done) {
            for (int n = 0; n < 500 && !done(feed->Latest()); ++n)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return feed->Latest();
        };
        feed->SetOpen(true);
        auto shown = wait([](const TournamentFeed::Snapshot& s) { return s.loaded; });
        CHECK(shown.signed_in && shown.page && shown.page->tournament.id == 4 && shown.page->me
              && shown.page->me->set == 30 && !shown.match && shown.error.empty());
        feed->Ready(true);
        shown = wait([](const TournamentFeed::Snapshot& s) { return !s.busy && s.match.has_value(); });
        CHECK(!shown.busy && shown.action_error.empty() && shown.match && shown.match->id == 12
              && shown.match->you_ready && !shown.match->opponent_ready);
        CHECK(feed->ReadyMatch() == 12);
        feed->Show(5);
        shown = wait([](const TournamentFeed::Snapshot& s) { return s.page && s.page->tournament.id == 5; });
        CHECK(shown.page && shown.page->tournament.id == 5 && shown.tournaments.size() == 2
              && shown.tournaments[1].place == 2 && shown.tournaments[0].place == 0);
        shown = wait([](const TournamentFeed::Snapshot& s) { return s.own && s.match; });
        CHECK(shown.own && shown.own->tournament.id == 4 && shown.match && shown.match->you_ready);
        feed->Ready(false);
        shown = wait([](const TournamentFeed::Snapshot& s) { return !s.busy && s.match && !s.match->you_ready; });
        CHECK(shown.action_error.empty() && shown.match && !shown.match->you_ready && !shown.error.empty()
              && shown.page && shown.page->tournament.id == 5 && shown.own && shown.own->tournament.id == 4);
        feed->Show(0);
        shown = wait([](const TournamentFeed::Snapshot& s) { return s.tournaments.size() == 2 && !s.own; });
        CHECK(shown.page && shown.page->tournament.id == 4 && shown.page->tournament.state == "cancelled"
              && !shown.own && shown.error.empty());
    }
    std::filesystem::remove_all(folder);
    CHECK(sent.size() == exchanges.size());
    for (const auto& request : sent) CHECK(request.expected);
    if (sent.size() == exchanges.size())
        CHECK(sent[0].authorization == "Bearer t9" && sent[2].body == R"({"ready":true})"
              && sent[10].body == R"({"ready":false})");
}

int main()
{
    Addresses();
    Sessions();
    Rules();
    LastOnline();
#ifndef _WIN32
    Requests();
    MiiAccounts();
    WfcSignIn();
    KnownWfc();
    FeedDiscordFriends();
    FeedChallengeRules();
    Tournaments();
#endif
    TournamentFeedRounds();
    if (failures)
    {
        std::fprintf(stderr, "%d profile service checks failed\n", failures);
        return 1;
    }
    std::printf("Profile service checks passed\n");
    return 0;
}
