#include "platform/profile_service.h"

#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <ctime>
#include <fstream>
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
    for (const auto& profile : value["wfc"].Items()) player.wfc.push_back(ParseWfc(profile));
    player.discord_id = value["discord_id"].Text();
    player.online = value["online"].Bool();
    player.busy = value["busy"].Bool();
    player.activity = value["activity"].Text();
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
    return match;
}

SignInResult ParseSignIn(const JsonValue& value)
{
    SignInResult result;
    const std::string& status = value["status"].Text();
    if (status == "signed_in")
    {
        result.status = SignInResult::Status::SignedIn;
        result.token = value["token"].Text();
        result.player = ParsePlayer(value["player"]);
        if (result.token.empty()) throw ProfileError("The server sent no session", 200);
    }
    else if (status == "refused")
    {
        result.status = SignInResult::Status::Refused;
        result.message = value["message"].Text();
    }
    return result;
}
} // namespace

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
        if (e.Status() != 404) throw;
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

ProfileMatch ProfileService::ChallengePlayer(std::int64_t opponent_id, std::int64_t rule_set)
{
    const std::string body = "{\"opponent_id\":" + Integer(opponent_id) + ",\"rule_set\":" + Integer(rule_set) + "}";
    return ParseMatch(Call("POST", "/api/v1/matches", &body).body);
}

std::optional<ProfileMatch> ProfileService::CurrentMatch(int* poll_interval)
{
    const auto answer = Call("GET", "/api/v1/matches/current", nullptr);
    if (poll_interval)
        if (const auto interval = answer.body["poll_interval"].Integer(); interval && *interval >= 1 && *interval <= 60)
            *poll_interval = int(*interval);
    const auto& match = answer.body["match"];
    if (!match.IsObject()) return std::nullopt;
    return ParseMatch(match);
}

PlayerLists ProfileService::Players()
{
    const auto answer = Call("GET", "/api/v1/players", nullptr);
    PlayerLists lists;
    lists.online = ParsePlayers(answer.body["online"]);
    lists.recent = ParsePlayers(answer.body["recent"]);
    lists.friends = ParsePlayers(answer.body["friends"]);
    return lists;
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

std::optional<SavedProfileSession> LoadProfileSession(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::nullopt;
    std::ostringstream text;
    text << stream.rdbuf();
    try
    {
        const auto document = ParseJson(text.str());
        SavedProfileSession session;
        session.server = document["server"].Text();
        session.token = document["token"].Text();
        session.player_name = document["player"].Text();
        if (session.server.empty() || session.token.empty()) return std::nullopt;
        return session;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
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

void ForgetProfileSession(const std::filesystem::path& file)
{
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}
} // namespace mscharged::platform
