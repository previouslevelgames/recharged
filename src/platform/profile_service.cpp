#include "platform/profile_service.h"

#include "platform/name_glyphs.h"

#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mscharged::platform
{
namespace
{
std::string Lower(std::string_view text)
{
    std::string lower(text);
    for (auto& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

// Four decimal parts of 0-255, or nullopt.
std::optional<std::array<int, 4>> Ipv4(std::string_view host)
{
    std::array<int, 4> parts{};
    for (std::size_t n = 0; n < parts.size(); ++n)
    {
        const std::size_t dot = host.find('.');
        const std::string_view part = host.substr(0, dot);
        if (part.empty() || part.size() > 3 || (n < 3) != (dot != std::string_view::npos)) return std::nullopt;
        int value = 0;
        const auto [last, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (error != std::errc() || last != part.data() + part.size() || value > 255) return std::nullopt;
        parts[n] = value;
        host = dot == std::string_view::npos ? std::string_view() : host.substr(dot + 1);
    }
    return parts;
}

// This computer or a private network, as the server's own check for its
// PUBLIC_URL: loopback, private and link-local IPv4, IPv6 loopback and
// unique local addresses, and localhost.
bool LocalHost(const std::string& host)
{
    if (host == "localhost") return true;
    if (const auto ip = Ipv4(host))
    {
        const auto& p = *ip;
        return p[0] == 127 || p[0] == 10 || (p[0] == 172 && p[1] >= 16 && p[1] <= 31) || (p[0] == 192 && p[1] == 168)
            || (p[0] == 169 && p[1] == 254);
    }
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    {
        const std::string address = host.substr(1, host.size() - 2);
        if (address == "::1") return true;
        // fc00::/7: the first group is fc.. or fd..
        unsigned group = 0;
        const char* end = address.data() + address.size();
        const auto [last, error] = std::from_chars(address.data(), end, group, 16);
        return error == std::errc() && last != address.data() && last != end && *last == ':' && group <= 0xffff
            && (group & 0xfe00) == 0xfc00;
    }
    return false;
}

std::string Integer(std::int64_t value) { return std::to_string(value); }

WfcProfile ParseWfc(const JsonValue& value)
{
    WfcProfile profile;
    profile.profile_id = value["profile_id"].Integer().value_or(0);
    profile.game = value["game"].Text();
    profile.friend_code = value["friend_code"].Text();
    profile.mii = value["mii"].Text();
    return profile;
}

ProfilePlayer ParsePlayer(const JsonValue& value)
{
    ProfilePlayer player;
    player.id = value["id"].Integer().value_or(0);
    player.name = value["name"].Text();
    player.discord = value["discord"].Text();
    player.avatar_url = value["avatar_url"].Text();
    player.test = value["test"].Bool();
    player.account = value["account"].Text();
    player.friend_code = value["friend_code"].Text();
    for (const auto& profile : value["wfc"].Items()) player.wfc.push_back(ParseWfc(profile));
    player.discord_id = value["discord_id"].Text();
    player.available = value["available"].Bool();
    player.online = value["online"].Bool();
    player.busy = value["busy"].Bool();
    player.activity = value["activity"].Text();
    player.match_kind = value["match"].Text();
    player.game = value["game"].Text();
    player.country = value["country"].Text();
    player.country_detected = value["country_detected"].Text();
    player.country_choice = value["country_choice"].Text();
    player.last_online = value["last_online"].Integer();
    player.is_friend = value["friend"].Bool();
    player.chat = value["chat"].Bool();
    player.unread = int(value["unread"].Integer().value_or(0));
    return player;
}

ChatMessage ParseChatMessage(const JsonValue& value)
{
    ChatMessage message;
    message.id = value["id"].Integer().value_or(0);
    message.mine = value["mine"].Bool();
    message.body = value["body"].Text();
    message.sent_at = value["sent_at"].Integer().value_or(0);
    return message;
}

std::vector<MiiProfile> ParseMiis(const JsonValue& value)
{
    std::vector<MiiProfile> miis;
    for (const auto& item : value.Items())
    {
        MiiProfile mii;
        mii.profile_id = std::int32_t(item["profile_id"].Integer().value_or(0));
        mii.mii = item["mii"].Text();
        mii.game = item["game"].Text();
        mii.friend_code = item["friend_code"].Text();
        mii.online = item["online"].Bool();
        mii.status = item["status"].Text();
        mii.match_kind = item["match"].Text();
        mii.last_online = item["last_online"].Integer();
        mii.is_friend = item["friend"].Bool();
        if (mii.profile_id) miis.push_back(std::move(mii));
    }
    return miis;
}

std::vector<ProfilePlayer> ParsePlayers(const JsonValue& value)
{
    std::vector<ProfilePlayer> players;
    for (const auto& player : value.Items()) players.push_back(ParsePlayer(player));
    return players;
}

MatchRules ParseRules(const JsonValue& value)
{
    MatchRules rules;
    for (const auto& tweak : value["tweaks"].Items()) rules.tweaks.push_back(tweak.Text());
    rules.skill = value["skill"].Text();
    rules.limit = value["limit"].Text();
    rules.time = int(value["time"].Integer().value_or(0));
    rules.goals = int(value["goals"].Integer().value_or(0));
    rules.games = int(value["games"].Integer().value_or(0));
    rules.stadium = value["stadium"].Text();
    for (auto [field, cheat] : {std::pair{"stadium_cheat", &rules.stadium_cheat},
                                std::pair{"player_cheat", &rules.player_cheat},
                                std::pair{"powerup_cheat", &rules.powerup_cheat}})
        if (const std::string text = value[field].Text(); !text.empty()) *cheat = text;
    return rules;
}

MatchGame ParseGame(const JsonValue& value)
{
    MatchGame game;
    game.my_score = int(value["my_score"].Integer().value_or(0));
    game.opponent_score = int(value["opponent_score"].Integer().value_or(0));
    game.my_captain = value["my_captain"].Text();
    game.opponent_captain = value["opponent_captain"].Text();
    game.stadium = value["stadium"].Text();
    return game;
}

ProfileMatch ParseMatch(const JsonValue& value)
{
    ProfileMatch match;
    match.id = value["id"].Integer().value_or(0);
    match.state = value["state"].Text();
    match.side = value["side"].Text();
    match.opponent = ParsePlayer(value["opponent"]);
    match.rule_set = value["rule_set"].Text();
    match.rules = ParseRules(value["rules"]);
    match.rules_hash = value["rules_hash"].Text();
    match.paired = value["paired"].Bool();
    match.reported = value["reported"].Bool();
    match.winner = value["winner"].Text();
    for (const auto& game : value["games"].Items()) match.games.push_back(ParseGame(game));
    match.created_at = value["created_at"].Integer().value_or(0);
    match.stage = value["stage"].Text();
    match.reason = value["reason"].Text();
    match.you_ready = value["you_ready"].Bool();
    match.opponent_ready = value["opponent_ready"].Bool();
    match.opponent_online = value["opponent_online"].Bool();
    match.go_at = value["go_at"].Integer().value_or(0);
    match.playing_at = value["playing_at"].Integer().value_or(0);
    match.rematch_of = value["rematch_of"].Integer().value_or(0);
    match.random = value["random"].Bool();
    return match;
}

SignInResult ParseSignIn(const JsonValue& value)
{
    SignInResult result;
    const std::string& status = value["status"].Text();
    // A Mii sign-in's answer may carry the session alone.
    if (status == "signed_in" || (status.empty() && !value["token"].Text().empty()))
    {
        result.status = SignInResult::Status::SignedIn;
        result.token = value["token"].Text();
        result.player = ParsePlayer(value["player"]);
        if (result.token.empty()) throw ProfileError("The server sent no session", 200);
        const auto& discord = value["discord"];
        if (!discord["access_token"].Text().empty() && !discord["refresh_token"].Text().empty())
        {
            DiscordTokens tokens;
            tokens.access_token = discord["access_token"].Text();
            tokens.refresh_token = discord["refresh_token"].Text();
            tokens.expires_in = discord["expires_in"].Integer().value_or(0);
            result.discord = std::move(tokens);
        }
    }
    else if (status == "refused")
    {
        result.status = SignInResult::Status::Refused;
        result.message = value["message"].Text();
    }
    return result;
}
} // namespace

std::string RulesJson(const MatchRules& rules)
{
    std::string json = "{\"tweaks\":[";
    for (std::size_t n = 0; n < rules.tweaks.size(); ++n)
    {
        if (n) json += ",";
        AppendJsonString(json, rules.tweaks[n]);
    }
    json += "],\"skill\":";
    AppendJsonString(json, rules.skill.empty() ? std::string("rookie") : rules.skill);
    json += ",\"limit\":";
    AppendJsonString(json, rules.limit);
    json += ",\"time\":" + Integer(rules.time) + ",\"goals\":" + Integer(rules.goals) + ",\"games\":" + Integer(rules.games)
        + ",\"stadium\":";
    AppendJsonString(json, rules.stadium);
    for (const auto& [field, value] : {std::pair{",\"stadium_cheat\":", &rules.stadium_cheat},
                                       std::pair{",\"player_cheat\":", &rules.player_cheat},
                                       std::pair{",\"powerup_cheat\":", &rules.powerup_cheat}})
    {
        json += field;
        AppendJsonString(json, *value);
    }
    return json + "}";
}

std::optional<std::string> ProfileServerAddress(std::string_view text, std::string* error)
{
    auto fail = [error](const char* message) -> std::optional<std::string> {
        if (error) *error = message;
        return std::nullopt;
    };
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (!text.empty() && text.back() == '/') text.remove_suffix(1);
    const std::string address = Lower(text);
    const bool https = address.rfind("https://", 0) == 0, http = address.rfind("http://", 0) == 0;
    if (!https && !http) return fail("Give the server's address with https://, e.g. https://wfc.example.com");
    const std::string authority = address.substr(https ? 8 : 7);
    if (authority.empty() || authority.find_first_of("/?#@ \\") != std::string::npos)
        return fail("Give only the server's address, e.g. https://wfc.example.com");
    // host, [IPv6] or either with :port
    std::string host = authority, port;
    if (const auto colon = authority.rfind(':');
        colon != std::string::npos && authority.find(']', colon) == std::string::npos)
    {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
        unsigned number = 0;
        const auto [last, failed] = std::from_chars(port.data(), port.data() + port.size(), number);
        if (port.empty() || failed != std::errc() || last != port.data() + port.size() || number == 0
            || number > 65535)
            return fail("The server's port is not a number from 1 to 65535");
        if ((https && number == 443) || (http && number == 80)) port.clear();
    }
    if (host.empty() || (host.front() == '[') != (host.back() == ']'))
        return fail("The server's address is not complete");
    if (http && !LocalHost(host))
        return fail("Use https:// (http:// works only for a server on this computer or network)");
    return (https ? "https://" : "http://") + host + (port.empty() ? "" : ":" + port);
}

struct ProfileService::Answer
{
    long status = 0;
    JsonValue body;
};

ProfileService::ProfileService(std::string server, std::string token)
    : server_(std::move(server)), token_(std::move(token))
{
    if (ProfileServerAddress(server_) != server_) throw ProfileError("Not a profile server address: " + server_, 0);
}

namespace
{
std::mutex gPlayedGameMutex;
std::string gPlayedGame; // SetPlayedGame
} // namespace

void ProfileService::SetPlayedGame(std::string game)
{
    if (game.size() > 4) game.resize(4);
    std::lock_guard<std::mutex> lock(gPlayedGameMutex);
    gPlayedGame = std::move(game);
}

ProfileService::Answer ProfileService::Call(const char* method, const std::string& path, const std::string* body)
{
    HttpRequest request;
    request.url = server_ + path;
    request.method = method;
    // ProfileServerAddress let http:// through only for local addresses.
    request.allow_http = server_.rfind("http://", 0) == 0;
    request.headers.push_back("Accept: application/json");
    if (body)
    {
        request.body = *body;
        request.headers.push_back("Content-Type: application/json");
    }
    if (!token_.empty()) request.headers.push_back("Authorization: Bearer " + token_);
    if (!activity_.empty()) request.headers.push_back("Recharged-Activity: " + activity_);
    if (!match_kind_.empty()) request.headers.push_back("Recharged-Match: " + match_kind_);
    {
        std::lock_guard<std::mutex> lock(gPlayedGameMutex);
        if (!gPlayedGame.empty()) request.headers.push_back("Recharged-Game: " + gPlayedGame);
    }
    request.connect_timeout = 10;
    request.stall_timeout = 20;
    request.max_size = 1u << 20;
    request.cancel = cancel_;
    HttpResponse response;
    try
    {
        response = HttpGet(request);
    }
    catch (const HttpError& e)
    {
        throw ProfileError(e.Cancelled() ? "Cancelled" : std::string("No answer from the server: ") + e.what(), 0,
                           e.Cancelled());
    }
    // Names in it (players, Miis, leaderboards, chats) may need characters
    // the shipped font lacks: the fonts add them.
    NoteNameGlyphs(response.body);
    Answer answer;
    answer.status = response.status;
    const bool success = response.status >= 200 && response.status < 300;
    if (!response.body.empty())
    {
        try
        {
            answer.body = ParseJson(response.body);
        }
        catch (const std::exception&)
        {
            if (success) throw ProfileError("The server's answer is not understood", response.status);
        }
    }
    if (!success)
    {
        std::string message = answer.body["error"].Text();
        if (message.empty()) message = "The server answered " + std::to_string(response.status);
        else message[0] = char(std::toupper(static_cast<unsigned char>(message[0])));
        throw ProfileError(message, response.status);
    }
    return answer;
}

ProfileServerInfo ProfileService::Info()
{
    try
    {
        const auto answer = Call("GET", "/api/v1/server", nullptr);
        ProfileServerInfo info;
        info.version = answer.body["version"].Text();
        info.discord = answer.body["sign_in"]["discord"].Bool();
        info.test = answer.body["sign_in"]["test"].Bool();
        info.mii = answer.body["sign_in"]["mii"].Bool();
        info.discord_application = answer.body["discord_application"].Text();
        return info;
    }
    catch (const ProfileError& e)
    {
        if (e.Status() == 404) throw ProfileError("This server has no profile service", 404);
        throw;
    }
}

SignInStart ProfileService::StartSignIn()
{
    const std::string empty = "{}";
    const auto answer = Call("POST", "/api/v1/sign-in", &empty);
    SignInStart start;
    start.id = answer.body["id"].Text();
    start.code = answer.body["code"].Text();
    start.url = answer.body["url"].Text();
    start.expires_in = int(answer.body["expires_in"].Integer().value_or(600));
    start.poll_interval = std::clamp(int(answer.body["poll_interval"].Integer().value_or(2)), 1, 30);
    start.link = answer.body["link"].Bool();
    if (start.id.empty() || start.url.empty()) throw ProfileError("The server's answer is not understood", 200);
    // Only a web page goes to the browser, never another kind of link.
    if (start.url.rfind("https://", 0) != 0 && start.url.rfind("http://", 0) != 0)
        throw ProfileError("The server's answer is not understood", 200);
    for (const char c : start.id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            throw ProfileError("The server's answer is not understood", 200);
    return start;
}

SignInResult ProfileService::PollSignIn(const std::string& id)
{
    try
    {
        return ParseSignIn(Call("GET", "/api/v1/sign-in/" + id, nullptr).body);
    }
    catch (const ProfileError& e)
    {
        // Refused in an error's words: a Discord account that has a profile
        // of its own, when connecting it to a Mii player's.
        const long code = e.Status();
        if (code == 400 || code == 403 || code == 409)
        {
            SignInResult result;
            result.status = SignInResult::Status::Refused;
            result.message = e.what();
            return result;
        }
        if (code != 404) throw;
        SignInResult result;
        result.status = SignInResult::Status::Expired;
        return result;
    }
}

SignInResult ProfileService::TestSignIn(const std::string& name)
{
    std::string body = "{\"name\":";
    AppendJsonString(body, name);
    body += '}';
    return ParseSignIn(Call("POST", "/api/v1/sign-in/test", &body).body);
}

SignInResult ProfileService::MiiSignIn(const std::string& name)
{
    std::string body = "{\"name\":";
    AppendJsonString(body, name);
    body += '}';
    return ParseSignIn(Call("POST", "/api/v1/sign-in/mii", &body).body);
}

SignInResult ProfileService::WfcSignIn(std::uint32_t profile_id, const std::string& login_ticket)
{
    std::string body = "{\"profile_id\":" + std::to_string(profile_id) + ",\"login_ticket\":";
    AppendJsonString(body, login_ticket);
    body += '}';
    return ParseSignIn(Call("POST", "/api/v1/sign-in/wfc", &body).body);
}

std::vector<std::pair<std::uint32_t, std::string>> ProfileService::KnownWfc(
    const std::vector<std::uint32_t>& profile_ids)
{
    std::string body = "{\"profile_ids\":[";
    for (std::size_t i = 0; i < profile_ids.size(); ++i)
        body += (i ? "," : "") + std::to_string(profile_ids[i]);
    body += "]}";
    const auto answer = Call("POST", "/api/v1/sign-in/wfc/known", &body);
    std::vector<std::pair<std::uint32_t, std::string>> known;
    for (const auto& profile : answer.body["profiles"].Items())
    {
        const auto id = profile["profile_id"].Integer().value_or(0);
        if (id > 0 && id <= 0xFFFFFFFFll) known.emplace_back(std::uint32_t(id), profile["sign_in"].Text());
    }
    return known;
}

std::vector<RuleSet> ProfileService::RuleSets()
{
    std::vector<RuleSet> sets;
    const auto answer = Call("GET", "/api/v1/rules", nullptr);
    for (const auto& value : answer.body["rule_sets"].Items())
    {
        RuleSet set;
        set.id = value["id"].Integer().value_or(0);
        set.name = value["name"].Text();
        set.rules = ParseRules(value["rules"]);
        set.hash = value["hash"].Text();
        sets.push_back(std::move(set));
    }
    return sets;
}

ProfilePlayer ProfileService::Me() { return ParsePlayer(Call("GET", "/api/v1/me", nullptr).body); }

void ProfileService::SignOut()
{
    const std::string empty = "{}";
    Call("POST", "/api/v1/sign-out", &empty);
}

ProfileMatch ProfileService::Challenge(const std::string& opponent, std::int64_t rule_set)
{
    std::string body = "{\"opponent\":";
    AppendJsonString(body, opponent);
    body += ",\"rule_set\":" + Integer(rule_set) + "}";
    return ParseMatch(Call("POST", "/api/v1/matches", &body).body);
}

ProfileMatch ProfileService::ChallengePlayer(std::int64_t opponent_id, const MatchRules& rules, const std::string& name)
{
    std::string body = "{\"opponent_id\":" + Integer(opponent_id) + ",\"rules\":" + RulesJson(rules);
    if (!name.empty())
    {
        body += ",\"name\":";
        AppendJsonString(body, name);
    }
    body += "}";
    return ParseMatch(Call("POST", "/api/v1/matches", &body).body);
}

ProfileMatch ProfileService::ChallengePlayer(std::int64_t opponent_id, std::int64_t rule_set)
{
    const std::string body = "{\"opponent_id\":" + Integer(opponent_id) + ",\"rule_set\":" + Integer(rule_set) + "}";
    return ParseMatch(Call("POST", "/api/v1/matches", &body).body);
}

static ProfileSearch ParseSearch(const JsonValue& value)
{
    ProfileSearch search;
    search.rule_set = value["rule_set"].Text();
    search.rules = ParseRules(value["rules"]);
    search.since = value["since"].Integer().value_or(0);
    search.asked = int(value["asked"].Integer().value_or(0));
    return search;
}

std::optional<ProfileMatch> ProfileService::CurrentMatch(int* poll_interval, std::optional<ProfileSearch>* search,
                                                         std::int64_t* server_time)
{
    const auto answer = Call("GET", "/api/v1/matches/current", nullptr);
    if (poll_interval)
        if (const auto interval = answer.body["poll_interval"].Integer(); interval && *interval >= 1 && *interval <= 60)
            *poll_interval = int(*interval);
    if (search)
    {
        search->reset();
        if (answer.body["search"].IsObject()) *search = ParseSearch(answer.body["search"]);
    }
    if (server_time) *server_time = answer.body["server_time"].Integer().value_or(0);
    const auto& match = answer.body["match"];
    if (!match.IsObject()) return std::nullopt;
    return ParseMatch(match);
}

ProfileSearch ProfileService::StartSearch(std::int64_t rule_set, const MatchRules& rules, const std::string& name,
                                          const std::string& game)
{
    std::string body = rule_set > 0 ? "{\"rule_set\":" + Integer(rule_set) : "{\"rules\":" + RulesJson(rules);
    if (rule_set <= 0 && !name.empty())
    {
        body += ",\"name\":";
        AppendJsonString(body, name);
    }
    body += ",\"game\":";
    AppendJsonString(body, game);
    body += "}";
    return ParseSearch(Call("POST", "/api/v1/search", &body).body["search"]);
}

void ProfileService::StopSearch() { Call("DELETE", "/api/v1/search", nullptr); }

Leaderboard ProfileService::RankedLeaderboard()
{
    const auto answer = Call("GET", "/api/v1/leaderboard", nullptr);
    const auto entry = [](const JsonValue& value) {
        LeaderboardEntry row;
        row.rank = int(value["rank"].Integer().value_or(0));
        row.mii = value["mii"].Text();
        row.player = value["player"].Text();
        if (const auto wins = value["wins"].Integer()) row.wins = int(*wins);
        if (const auto losses = value["losses"].Integer()) row.losses = int(*losses);
        row.points = int(value["points"].Integer().value_or(0));
        row.region = int(value["region"].Integer().value_or(0));
        row.you = value["you"].Bool();
        return row;
    };
    const auto board = [&](const JsonValue& value) {
        LeaderboardBoard result;
        result.first_day = value["first_day"].Text();
        result.last_day = value["last_day"].Text();
        result.players = int(value["players"].Integer().value_or(0));
        for (const auto& row : value["top"].Items()) result.top.push_back(entry(row));
        if (value["you"].IsObject()) result.you = entry(value["you"]);
        return result;
    };
    Leaderboard leaderboard;
    leaderboard.season = board(answer.body["season"]);
    leaderboard.daily = board(answer.body["daily"]);
    return leaderboard;
}

PlayerLists ProfileService::Players()
{
    const auto answer = Call("GET", "/api/v1/players", nullptr);
    PlayerLists lists;
    lists.online = ParsePlayers(answer.body["online"]);
    lists.recent = ParsePlayers(answer.body["recent"]);
    lists.friends = ParsePlayers(answer.body["friends"]);
    lists.available = ParsePlayers(answer.body["available"]);
    lists.mii_friends = ParseMiis(answer.body["mii_friends"]);
    lists.game_online = ParseMiis(answer.body["game_online"]);
    return lists;
}

std::vector<MiiProfile> ProfileService::AddMiiFriend(std::int32_t profile_id)
{
    const std::string body = "{\"profile_id\":" + Integer(profile_id) + "}";
    return ParseMiis(Call("POST", "/api/v1/friends/miis", &body).body["mii_friends"]);
}

std::vector<MiiProfile> ProfileService::RemoveMiiFriend(std::int32_t profile_id)
{
    return ParseMiis(Call("DELETE", "/api/v1/friends/miis/" + Integer(profile_id), nullptr).body["mii_friends"]);
}

bool ProfileService::SetAvailable(bool available)
{
    const std::string body = std::string("{\"available\":") + (available ? "true" : "false") + "}";
    return Call("PUT", "/api/v1/me/available", &body).body["available"].Bool();
}

ProfilePlayer ProfileService::ChooseCountry(const std::string& choice)
{
    std::string body = "{\"country\":";
    AppendJsonString(body, choice);
    body += '}';
    return ParsePlayer(Call("PUT", "/api/v1/me/country", &body).body);
}

std::vector<ProfilePlayer> ProfileService::AddFriend(std::int64_t player_id)
{
    const std::string body = "{\"player_id\":" + Integer(player_id) + "}";
    return ParsePlayers(Call("POST", "/api/v1/friends", &body).body["friends"]);
}

std::vector<ProfilePlayer> ProfileService::AddFriend(const std::string& name)
{
    std::string body = "{\"name\":";
    AppendJsonString(body, name);
    body += "}";
    return ParsePlayers(Call("POST", "/api/v1/friends", &body).body["friends"]);
}

std::vector<ProfilePlayer> ProfileService::RemoveFriend(std::int64_t player_id)
{
    return ParsePlayers(Call("DELETE", "/api/v1/friends/" + Integer(player_id), nullptr).body["friends"]);
}

namespace
{
DiscordTokens ParseDiscordTokens(const JsonValue& value)
{
    DiscordTokens tokens;
    tokens.access_token = value["access_token"].Text();
    tokens.refresh_token = value["refresh_token"].Text();
    tokens.expires_in = value["expires_in"].Integer().value_or(0);
    if (tokens.access_token.empty() || tokens.refresh_token.empty())
        throw ProfileError("The server's Discord tokens are incomplete", 0);
    return tokens;
}
} // namespace

DiscordTokens ProfileService::DiscordToken(const std::string& code, const std::string& verifier,
                                           const std::string& redirect_uri)
{
    std::string body = "{\"code\":";
    AppendJsonString(body, code);
    body += ",\"code_verifier\":";
    AppendJsonString(body, verifier);
    body += ",\"redirect_uri\":";
    AppendJsonString(body, redirect_uri);
    body += "}";
    return ParseDiscordTokens(Call("POST", "/api/v1/discord/token", &body).body);
}

DiscordTokens ProfileService::RefreshDiscordToken(const std::string& refresh_token)
{
    std::string body = "{\"refresh_token\":";
    AppendJsonString(body, refresh_token);
    body += "}";
    return ParseDiscordTokens(Call("POST", "/api/v1/discord/token", &body).body);
}

std::vector<ProfilePlayer> ProfileService::PlayersByDiscord(const std::vector<std::string>& discord_ids)
{
    std::string body = "{\"ids\":[";
    for (std::size_t i = 0; i < discord_ids.size(); ++i)
    {
        if (i) body += ',';
        AppendJsonString(body, discord_ids[i]);
    }
    body += "]}";
    return ParsePlayers(Call("POST", "/api/v1/players/discord", &body).body["players"]);
}

std::vector<ProfilePlayer> FindDiscordPlayers(ProfileService& api, const std::vector<std::string>& discord_ids)
{
    // As many at once as the server takes.
    constexpr std::size_t kPart = 200;
    const std::size_t count = std::min(discord_ids.size(), kDiscordFriendsLookedUp);
    std::vector<ProfilePlayer> found;
    try
    {
        for (std::size_t first = 0; first < count; first += kPart)
        {
            const std::vector<std::string> part(discord_ids.begin() + std::ptrdiff_t(first),
                                                discord_ids.begin() + std::ptrdiff_t(std::min(count, first + kPart)));
            for (auto& player : api.PlayersByDiscord(part)) found.push_back(std::move(player));
        }
    }
    catch (const ProfileError& e)
    {
        // A server before 1.2.0 can't tell.
        if (e.Status() != 404) throw;
    }
    std::sort(found.begin(), found.end(), [](const ProfilePlayer& a, const ProfilePlayer& b) {
        if (a.online != b.online) return a.online;
        return a.name < b.name;
    });
    return found;
}

ChatPage ProfileService::Chat(std::int64_t player_id, std::int64_t after)
{
    std::string path = "/api/v1/chat/" + Integer(player_id);
    if (after > 0) path += "?after=" + Integer(after);
    const auto answer = Call("GET", path, nullptr);
    ChatPage page;
    for (const auto& message : answer.body["messages"].Items()) page.messages.push_back(ParseChatMessage(message));
    page.can_send = answer.body["can_send"].Bool();
    return page;
}

ChatMessage ProfileService::SendChat(std::int64_t player_id, const std::string& body)
{
    std::string request = "{\"body\":";
    AppendJsonString(request, body);
    request += "}";
    return ParseChatMessage(Call("POST", "/api/v1/chat/" + Integer(player_id), &request).body);
}

std::string DescribeLastOnline(std::int64_t now, std::optional<std::int64_t> when)
{
    if (!when) return "Not online yet";
    const std::int64_t seconds = std::max<std::int64_t>(0, now - *when);
    const auto count = [](std::int64_t value, const char* unit) {
        return std::to_string(value) + " " + unit + (value == 1 ? "" : "s") + " ago";
    };
    std::string text;
    if (seconds < 120) text = "just now";
    else if (seconds < 3600) text = count(seconds / 60, "minute");
    else if (seconds < 86400) text = count(seconds / 3600, "hour");
    else if (seconds < 2 * 86400) text = "yesterday";
    else if (seconds < 30 * 86400) text = count(seconds / 86400, "day");
    else
    {
        // Longer ago: the day, and the year when it was another.
        const std::time_t then = std::time_t(*when), today = std::time_t(now);
        std::tm then_local{}, now_local{};
#ifdef _WIN32
        localtime_s(&then_local, &then);
        localtime_s(&now_local, &today);
#else
        localtime_r(&then, &then_local);
        localtime_r(&today, &now_local);
#endif
        char day[32];
        std::strftime(day, sizeof day, then_local.tm_year == now_local.tm_year ? "%d %B" : "%d %B %Y", &then_local);
        text = std::string("on ") + (day[0] == '0' ? day + 1 : day);
    }
    return "Last online " + text;
}

std::string DescribeChatTime(std::int64_t now, std::int64_t when)
{
    const auto local = [](std::int64_t seconds) {
        const std::time_t time = std::time_t(seconds);
        std::tm value{};
#ifdef _WIN32
        localtime_s(&value, &time);
#else
        localtime_r(&time, &value);
#endif
        return value;
    };
    const std::tm then = local(when), today = local(now), yesterday = local(now - 86400);
    char clock[16], day[40];
    std::strftime(clock, sizeof clock, "%H:%M", &then);
    const auto same_day = [&](const std::tm& a) { return a.tm_year == then.tm_year && a.tm_yday == then.tm_yday; };
    if (same_day(today)) return clock;
    if (same_day(yesterday)) return std::string("Yesterday ") + clock;
    std::strftime(day, sizeof day, then.tm_year == today.tm_year ? "%d %B" : "%d %B %Y", &then);
    return std::string(day[0] == '0' ? day + 1 : day) + ", " + clock;
}

ProfileMatch ProfileService::ReadyToPlay(std::int64_t match, bool ready)
{
    const std::string body = ready ? "{\"ready\":true}" : "{\"ready\":false}";
    return ParseMatch(Call("POST", "/api/v1/matches/" + Integer(match) + "/ready-to-play", &body).body);
}

ProfileMatch ProfileService::Rematch(std::int64_t match)
{
    const std::string empty = "{}";
    return ParseMatch(Call("POST", "/api/v1/matches/" + Integer(match) + "/rematch", &empty).body);
}

ProfileMatch ProfileService::Accept(std::int64_t match)
{
    const std::string empty = "{}";
    return ParseMatch(Call("POST", "/api/v1/matches/" + Integer(match) + "/accept", &empty).body);
}

ProfileMatch ProfileService::Decline(std::int64_t match)
{
    const std::string empty = "{}";
    return ParseMatch(Call("POST", "/api/v1/matches/" + Integer(match) + "/decline", &empty).body);
}

ProfileMatch ProfileService::Cancel(std::int64_t match)
{
    const std::string empty = "{}";
    return ParseMatch(Call("POST", "/api/v1/matches/" + Integer(match) + "/cancel", &empty).body);
}

WfcProfile ProfileService::ProveWfcLogin(std::int64_t profile_id, const std::string& login_ticket)
{
    std::string body = "{\"profile_id\":" + Integer(profile_id) + ",\"login_ticket\":";
    AppendJsonString(body, login_ticket);
    body += '}';
    return ParseWfc(Call("POST", "/api/v1/wfc", &body).body);
}

ReadyAnswer ProfileService::Ready(std::int64_t match, std::int64_t profile_id, const std::string& rules_hash)
{
    std::string body = "{\"profile_id\":" + Integer(profile_id) + ",\"rules_hash\":";
    AppendJsonString(body, rules_hash);
    body += '}';
    const auto answer = Call("POST", "/api/v1/matches/" + Integer(match) + "/ready", &body);
    ReadyAnswer ready;
    ready.paired = answer.body["paired"].Bool();
    ready.other_release = answer.body["other_release"].Bool();
    ready.replaced = answer.body["replaced"].Bool();
    return ready;
}

ProfileMatch ProfileService::Report(std::int64_t match, const std::vector<MatchGame>& games)
{
    std::string body = "{\"games\":[";
    for (std::size_t n = 0; n < games.size(); ++n)
    {
        const auto& game = games[n];
        if (n) body += ',';
        body += "{\"my_score\":" + std::to_string(game.my_score)
            + ",\"opponent_score\":" + std::to_string(game.opponent_score) + ",\"my_captain\":";
        AppendJsonString(body, game.my_captain);
        body += ",\"opponent_captain\":";
        AppendJsonString(body, game.opponent_captain);
        body += ",\"stadium\":";
        AppendJsonString(body, game.stadium);
        // Only a game that broke off says so: older servers refuse fields
        // they don't know.
        if (game.interrupted)
        {
            char played[32];
            std::snprintf(played, sizeof played, "%.3f", std::clamp(double(game.played), 0.0, 1.0));
            body += ",\"interrupted\":true,\"played\":";
            body += played;
        }
        body += '}';
    }
    body += "]}";
    return ParseMatch(Call("POST", "/api/v1/matches/" + Integer(match) + "/report", &body).body);
}

std::filesystem::path ProfileSessionFile(const std::filesystem::path& data_folder, const std::filesystem::path& config)
{
    auto name = config.stem();
    if (name.empty()) name = "recharged";
    return data_folder / "online" / name.concat(".session");
}

namespace
{
// Writes `text` to `file` through a new file that only this user can read
// (it holds a token) and a rename.
void WritePrivateFile(const std::filesystem::path& file, const std::string& text)
{
    std::filesystem::create_directories(file.parent_path());
    auto temporary = file;
    temporary += ".new";
#ifdef _WIN32
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.write(text.data(), std::streamsize(text.size())))
            throw std::runtime_error("Cannot write " + PathUtf8(temporary));
    }
#else
    // Made readable by this user only before the token is in it.
    std::filesystem::remove(temporary);
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) throw std::runtime_error("Cannot write " + PathUtf8(temporary));
    const bool written = ::write(descriptor, text.data(), text.size()) == ssize_t(text.size());
    if (::close(descriptor) != 0 || !written)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Cannot write " + PathUtf8(temporary));
    }
#endif
    std::filesystem::rename(temporary, file);
}

std::optional<JsonValue> ReadJsonFile(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream text;
    text << stream.rdbuf();
    try
    {
        return ParseJson(text.str());
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}
} // namespace

std::optional<SavedProfileSession> LoadProfileSession(const std::filesystem::path& file)
{
    const auto document = ReadJsonFile(file);
    if (!document) return std::nullopt;
    SavedProfileSession session;
    session.server = (*document)["server"].Text();
    session.token = (*document)["token"].Text();
    session.player_name = (*document)["player"].Text();
    if (session.server.empty() || session.token.empty()) return std::nullopt;
    return session;
}


void SaveProfileSession(const std::filesystem::path& file, const SavedProfileSession& session)
{
    std::string text = "{\"server\":";
    AppendJsonString(text, session.server);
    text += ",\"token\":";
    AppendJsonString(text, session.token);
    text += ",\"player\":";
    AppendJsonString(text, session.player_name);
    text += "}\n";
    WritePrivateFile(file, text);
}

std::filesystem::path KnownProfilesFile(const std::filesystem::path& data_folder,
                                        const std::filesystem::path& config)
{
    auto file = ProfileSessionFile(data_folder, config);
    return file.replace_extension(".profiles");
}

std::vector<KnownProfile> LoadKnownProfiles(const std::filesystem::path& file)
{
    std::vector<KnownProfile> profiles;
    const auto document = ReadJsonFile(file);
    if (!document) return profiles;
    for (const auto& value : (*document)["profiles"].Items())
    {
        KnownProfile profile;
        profile.server = value["server"].Text();
        profile.player_id = value["id"].Integer().value_or(0);
        profile.name = value["name"].Text();
        profile.account = value["account"].Text();
        for (const auto& code : value["friend_codes"].Items()) profile.friend_codes.push_back(code.Text());
        if (!profile.server.empty() && profile.player_id > 0) profiles.push_back(std::move(profile));
    }
    return profiles;
}

void SaveKnownProfiles(const std::filesystem::path& file, const std::vector<KnownProfile>& profiles)
{
    std::string text = "{\"profiles\":[";
    for (std::size_t n = 0; n < profiles.size(); ++n)
    {
        const auto& profile = profiles[n];
        text += n ? ",\n{\"server\":" : "\n{\"server\":";
        AppendJsonString(text, profile.server);
        text += ",\"id\":" + std::to_string(profile.player_id) + ",\"name\":";
        AppendJsonString(text, profile.name);
        text += ",\"account\":";
        AppendJsonString(text, profile.account);
        text += ",\"friend_codes\":[";
        for (std::size_t i = 0; i < profile.friend_codes.size(); ++i)
        {
            if (i) text += ',';
            AppendJsonString(text, profile.friend_codes[i]);
        }
        text += "]}";
    }
    text += "\n]}\n";
    WritePrivateFile(file, text);
}

std::filesystem::path DiscordLinkFile(const std::filesystem::path& data_folder, const std::filesystem::path& config)
{
    auto file = ProfileSessionFile(data_folder, config);
    return file.replace_extension(".discord");
}

std::optional<SavedDiscordLink> LoadDiscordLink(const std::filesystem::path& file)
{
    const auto document = ReadJsonFile(file);
    if (!document) return std::nullopt;
    SavedDiscordLink link;
    link.server = (*document)["server"].Text();
    link.refresh_token = (*document)["refresh_token"].Text();
    if (link.server.empty() || link.refresh_token.empty()) return std::nullopt;
    return link;
}

void SaveDiscordLink(const std::filesystem::path& file, const SavedDiscordLink& link)
{
    std::string text = "{\"server\":";
    AppendJsonString(text, link.server);
    text += ",\"refresh_token\":";
    AppendJsonString(text, link.refresh_token);
    text += "}\n";
    WritePrivateFile(file, text);
}

void ForgetProfileSession(const std::filesystem::path& file)
{
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}
} // namespace mscharged::platform
