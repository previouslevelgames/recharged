#include "platform/live_watch.h"

#include "platform/json.h"
#include "platform/match_replay_stream.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace mscharged::platform
{
namespace
{
// The server's frames (src/streams.rs there).
constexpr std::uint8_t kFrameKeepalive = 0, kFrameGame = 1, kFrameData = 2, kFrameEnd = 3, kFrameSkip = 4;
// A frame larger than this is no frame of the server's.
constexpr std::size_t kMostFrame = 16u << 20;
constexpr int kConnectTimeout = 5000;


// A GET of `path` with a watch ticket (out of the URL: request logs keep
// URLs): the answer's status, its head (names in lower case) and its body,
// at most `most` bytes. False with `error` set when there is no answer.
struct TicketAnswer
{
    int status = 0;
    std::string head;
    std::string body;

    // A field's value as it came (after "name:"), or null.
    const char* Field(const char* name) const
    {
        const auto at = head.find(std::string("\r\n") + name + ":");
        return at == std::string::npos ? nullptr : head.c_str() + at + 3 + std::strlen(name);
    }
};

bool TicketGet(const std::string& server, const std::string& path, const WatchTicket& ticket, const char* accept,
               std::size_t most, std::chrono::milliseconds timeout, TicketAnswer& answer, std::string& error)
{
    TcpClient connection;
    if (!connection.Connect(server, kConnectTimeout, error))
    {
        error = "The server cannot be reached: " + error;
        return false;
    }
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + server
        + "\r\nUser-Agent: Recharged\r\nAccept: " + accept + "\r\nAuthorization: Bearer " + ticket.ticket
        + "\r\nConnection: close\r\n\r\n";
    const auto until = std::chrono::steady_clock::now() + timeout;
    auto late = [&] { return std::chrono::steady_clock::now() > until; };
    for (std::size_t at = 0; at < request.size();)
    {
        const long sent = connection.SendSome(request.data() + at, request.size() - at);
        if (sent < 0 || late())
        {
            error = "The connection to the server broke.";
            return false;
        }
        at += std::size_t(sent);
        if (!sent) connection.WaitReadable(5);
    }
    // The whole answer: the server closes the connection after it.
    std::string whole;
    char buffer[16384];
    for (;;)
    {
        const long got = connection.ReceiveSome(buffer, sizeof buffer);
        if (got < 0) break;
        if (got > 0)
        {
            whole.append(buffer, std::size_t(got));
            if (whole.size() > most + (64u << 10))
            {
                error = "The server's answer is larger than expected.";
                return false;
            }
            continue;
        }
        if (late())
        {
            error = "The server did not answer in time.";
            return false;
        }
        connection.WaitReadable(100);
    }
    const auto end = whole.find("\r\n\r\n");
    answer.status = 0;
    if (whole.compare(0, 5, "HTTP/") == 0 && end != std::string::npos)
        if (const auto space = whole.find(' '); space < end) answer.status = std::atoi(whole.c_str() + space + 1);
    if (!answer.status)
    {
        error = "The server's answer cannot be read.";
        return false;
    }
    answer.head = whole.substr(0, end);
    std::transform(answer.head.begin(), answer.head.end(), answer.head.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    std::size_t length = whole.size() - end - 4;
    if (const char* value = answer.Field("content-length"))
    {
        const std::size_t said = std::size_t(std::strtoull(value, nullptr, 10));
        if (said > length)
        {
            error = "The server's answer came cut short.";
            return false;
        }
        length = said;
    }
    answer.body.assign(whole, end + 4, length);
    return true;
}

// The text of `value` (a JSON string), at most `most` bytes, cut at a
// character's start.
std::string Text(const JsonValue& value, std::size_t most)
{
    std::string text = value.Text();
    if (!value.IsString()) text.clear();
    if (text.size() > most)
    {
        std::size_t cut = most;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
    }
    return text;
}
} // namespace

bool FetchKeptReplay(const std::string& server, const WatchTicket& ticket, std::vector<std::uint8_t>& file,
                     int& next_game, std::string& error, std::chrono::milliseconds timeout)
{
    file.clear();
    next_game = 0;
    // The largest file the server keeps (1 MiB).
    TicketAnswer answer;
    if (!TicketGet(server, "/api/v1/wfc/replays/" + std::to_string(ticket.match) + "/" + std::to_string(ticket.game),
                   ticket, "application/octet-stream", 1u << 20, timeout, answer, error))
        return false;
    if (answer.status != 200)
    {
        error = answer.status == 404
            ? "The server keeps no replay of game " + std::to_string(ticket.game) + " of this match."
            : answer.status == 401 || answer.status == 403
            ? "The server does not take this ticket (a ticket lasts four hours, on the server that made it)."
            : "The server answered HTTP " + std::to_string(answer.status) + ".";
        return false;
    }
    if (const char* value = answer.Field("x-next-game"))
    {
        const long next = std::strtol(value, nullptr, 10);
        if (next > ticket.game && next <= 9) next_game = int(next);
    }
    file.assign(answer.body.begin(), answer.body.end());
    return true;
}

std::pair<int, int> WatchedMatch::WinsBefore(int game) const
{
    std::pair<int, int> wins{0, 0};
    for (const auto& played : games)
    {
        if (played.number >= game) continue;
        if (played.home > played.away) ++wins.first;
        else if (played.away > played.home) ++wins.second;
    }
    return wins;
}

bool ReadWatchedMatch(const std::string& json, WatchedMatch& match, std::string& error)
{
    try
    {
        const JsonValue root = ParseJson(json);
        if (!root.IsObject() || !root["home"].IsObject() || !root["away"].IsObject())
        {
            error = "The match's details are not an object with both sides.";
            return false;
        }
        WatchedMatch read;
        // Names as long as the menus take them; a flag only as two capital letters.
        constexpr std::size_t kMostName = 64;
        const auto side = [&](const JsonValue& value, WatchedMatch::Side& out) {
            out.name = Text(value["name"], kMostName);
            if (out.name.empty()) out.name = Text(value["mii"], kMostName);
            const std::string country = Text(value["country"], 2);
            if (country.size() == 2 && country[0] >= 'A' && country[0] <= 'Z' && country[1] >= 'A'
                && country[1] <= 'Z')
                out.country = country;
        };
        side(root["home"], read.home);
        side(root["away"], read.away);
        const auto best_of = root["best_of"].Integer();
        read.best_of = best_of && *best_of >= 1 && *best_of <= 9 ? int(*best_of) : 1;
        for (const auto& game : root["games"].Items())
        {
            const auto number = game["number"].Integer();
            const auto home = game["score"]["home"].Integer();
            const auto away = game["score"]["away"].Integer();
            if (!number || !home || !away || *number < 1 || *number > 9) continue;
            read.games.push_back({int(*number), int(std::clamp<std::int64_t>(*home, 0, 99)),
                                  int(std::clamp<std::int64_t>(*away, 0, 99))});
            if (read.games.size() >= 9) break;
        }
        match = std::move(read);
        return true;
    }
    catch (const std::exception& failure)
    {
        error = std::string("The match's details cannot be read: ") + failure.what();
        return false;
    }
}

bool FetchWatchedMatch(const std::string& server, const WatchTicket& ticket, WatchedMatch& match, bool& absent,
                       std::string& error, std::chrono::milliseconds timeout)
{
    absent = false;
    TicketAnswer answer;
    if (!TicketGet(server, "/api/v1/wfc/streams/" + std::to_string(ticket.match) + "/info", ticket,
                   "application/json", 64u << 10, timeout, answer, error))
        return false;
    if (answer.status != 200)
    {
        absent = answer.status == 404;
        error = absent ? "The server tells no details of this match (servers before 1.3.7 tell none)."
            : answer.status == 401 || answer.status == 403
            ? "The server does not take this ticket (a ticket lasts four hours, on the server that made it)."
            : "The server answered HTTP " + std::to_string(answer.status) + ".";
        return false;
    }
    return ReadWatchedMatch(answer.body, match, error);
}

LiveWatch::LiveWatch(std::string server, WatchTicket ticket, Log log)
    : server_(std::move(server)), ticket_(std::move(ticket)), log_(std::move(log))
{
    thread_ = std::thread([this] { Run(); });
}

LiveWatch::~LiveWatch()
{
    stop_ = true;
    thread_.join();
}

bool LiveWatch::WaitForGame(std::chrono::milliseconds timeout, int& game, std::vector<std::uint8_t>& start,
                            std::string& error)
{
    std::unique_lock lock(mutex_);
    mscharged::replay::Setup setup;
    std::string unread;
    const bool ready = changed_.wait_for(lock, timeout, [&] {
        return failed_ || over_
            || (game_ && !bytes_.empty() && mscharged::replay::ReadSetup(bytes_.data(), bytes_.size(), setup, unread));
    });
    if (game_ && mscharged::replay::ReadSetup(bytes_.data(), bytes_.size(), setup, unread))
    {
        game = game_;
        start = bytes_;
        given_ = bytes_.size();
        return true;
    }
    error = !error_.empty() ? error_
        : over_            ? "The match is over."
        : ready            ? "The match's stream cannot be read."
                           : "No game of this match is being played now.";
    return false;
}

void LiveWatch::Feed(ChargedFeedLiveReplayFunction feed, ChargedFeedLiveSkipFunction skip)
{
    std::lock_guard lock(mutex_);
    feed_ = feed;
    skip_ = skip;
    if (skip_)
        for (const auto& [part, after] : skips_) skip_(part, after);
    skips_.clear();
    if (!feed_) return;
    if (bytes_.size() > given_ || complete_)
        feed_(bytes_.data() + given_, unsigned(bytes_.size() - given_), complete_ ? 1 : 0);
    bytes_.clear();
    bytes_.shrink_to_fit();
    given_ = 0;
}

void LiveWatch::Frame(std::uint8_t kind, const std::uint8_t* data, std::size_t size)
{
    if (kind == kFrameKeepalive) return; // the server is still there
    std::lock_guard lock(mutex_);
    if (kind == kFrameGame && size >= 4)
    {
        const int game = int(data[0] | data[1] << 8 | data[2] << 16 | std::uint32_t(data[3]) << 24);
        if (!game_)
        {
            game_ = game;
            if (log_) log_("Watching: game " + std::to_string(game) + " of match " + std::to_string(ticket_.match));
        }
        else if (game != game_ && !next_game_)
        {
            next_game_ = true;
            if (log_) log_("Watching: game " + std::to_string(game) + " began");
        }
    }
    else if (kind == kFrameData && game_ && !next_game_)
    {
        if (feed_)
        {
            if (!feed_(data, unsigned(size), 0) && !failed_)
            {
                failed_ = true;
                if (log_) log_("Watching: the game's stream is larger than the replay can take");
            }
        }
        else
            bytes_.insert(bytes_.end(), data, data + size);
    }
    else if (kind == kFrameSkip && size >= 4 && game_ && !next_game_)
    {
        // A player skipped a part of a cutscene: its number, and when.
        const unsigned part = data[0] | data[1] << 8, after = data[2] | data[3] << 8;
        if (skip_) skip_(part, after);
        else if (skips_.size() < 512) skips_.emplace_back(part, after);
    }
    else if (kind == kFrameEnd && game_ && !next_game_)
    {
        complete_ = true;
        if (feed_) feed_(nullptr, 0, 1);
    }
    changed_.notify_all();
}

void LiveWatch::Run()
{
    auto fail = [&](const std::string& why) {
        std::lock_guard lock(mutex_);
        error_ = why;
        failed_ = true;
        changed_.notify_all();
        if (log_) log_("Watching: " + why);
    };
    std::string error;
    if (!connection_.Connect(server_, kConnectTimeout, error)) return fail("cannot reach the server: " + error);
    // The ticket as a header: request logs keep URLs.
    const std::string request = "GET /api/v1/wfc/streams/" + std::to_string(ticket_.match) + " HTTP/1.1\r\nHost: "
        + server_ + "\r\nUser-Agent: Recharged\r\nAccept: application/octet-stream\r\nAuthorization: Bearer "
        + ticket_.ticket + "\r\nConnection: close\r\n\r\n";
    for (std::size_t at = 0; at < request.size() && !stop_;)
    {
        const long sent = connection_.SendSome(request.data() + at, request.size() - at);
        if (sent < 0) return fail("the connection to the server broke");
        at += std::size_t(sent);
        if (!sent) connection_.WaitReadable(5);
    }
    // The answer's head, then its body: chunked (the server streams it) or
    // as it comes until the end.
    std::string in;
    bool headed = false, chunked = false, closed = false;
    std::vector<std::uint8_t> frames; // the body's bytes not yet a whole frame
    std::size_t chunk_left = 0;
    bool chunk_trailer = false; // the CRLF after a chunk's data
    auto take_body = [&](const char* data, std::size_t size) {
        frames.insert(frames.end(), reinterpret_cast<const std::uint8_t*>(data),
                      reinterpret_cast<const std::uint8_t*>(data) + size);
        std::size_t at = 0;
        while (frames.size() - at >= 5)
        {
            const std::uint8_t* frame = frames.data() + at;
            const std::size_t length =
                std::size_t(frame[1] | frame[2] << 8 | frame[3] << 16 | std::uint32_t(frame[4]) << 24);
            if (length > kMostFrame) return false;
            if (frames.size() - at - 5 < length) break;
            Frame(frame[0], frame + 5, length);
            at += 5 + length;
        }
        frames.erase(frames.begin(), frames.begin() + std::ptrdiff_t(at));
        return true;
    };
    char buffer[16384];
    while (!stop_)
    {
        if (!connection_.WaitReadable(250)) continue;
        const long got = connection_.ReceiveSome(buffer, sizeof buffer);
        if (got < 0)
        {
            closed = true;
            break;
        }
        if (!got) continue;
        in.append(buffer, std::size_t(got));
        if (!headed)
        {
            const auto end = in.find("\r\n\r\n");
            if (end == std::string::npos)
            {
                if (in.size() > 16384) return fail("the server's answer is not one");
                continue;
            }
            std::string head = in.substr(0, end);
            in.erase(0, end + 4);
            const int status = head.compare(0, 5, "HTTP/") == 0 ? std::atoi(head.c_str() + head.find(' ') + 1) : 0;
            std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            chunked = head.find("\r\ntransfer-encoding: chunked") != std::string::npos;
            if (status != 200)
            {
                // Its reason, from the JSON the server answers with.
                std::string reason;
                for (int wait = 0; wait < 20 && in.find('}') == std::string::npos; ++wait)
                {
                    connection_.WaitReadable(100);
                    const long more = connection_.ReceiveSome(buffer, sizeof buffer);
                    if (more < 0) break;
                    in.append(buffer, std::size_t(std::max(0L, more)));
                }
                const auto open = in.find('{');
                try
                {
                    if (open != std::string::npos) reason = ParseJson(in.substr(open, in.rfind('}') - open + 1))["error"].Text();
                }
                catch (const std::exception&)
                {
                }
                return fail(status == 403 ? "the ticket does not let you watch this match (it lasts four hours, "
                                            "on the server that made it)"
                            : status == 404 ? "the server streams no matches"
                                            : "the server refused (HTTP " + std::to_string(status) + ")"
                                    + (reason.empty() ? "" : ": " + reason));
            }
            headed = true;
        }
        // The body so far.
        if (!chunked)
        {
            if (!take_body(in.data(), in.size())) return fail("the match's stream cannot be read");
            in.clear();
            continue;
        }
        for (;;)
        {
            if (chunk_trailer)
            {
                if (in.size() < 2) break;
                in.erase(0, 2);
                chunk_trailer = false;
            }
            if (chunk_left)
            {
                const std::size_t part = std::min(chunk_left, in.size());
                if (!part) break;
                if (!take_body(in.data(), part)) return fail("the match's stream cannot be read");
                in.erase(0, part);
                chunk_left -= part;
                if (!chunk_left) chunk_trailer = true;
                continue;
            }
            const auto line = in.find("\r\n");
            if (line == std::string::npos) break;
            chunk_left = std::size_t(std::strtoull(in.c_str(), nullptr, 16));
            in.erase(0, line + 2);
            if (!chunk_left)
            {
                closed = true; // the last chunk: the stream ended
                break;
            }
        }
        if (closed) break;
    }
    if (stop_) return;
    std::lock_guard lock(mutex_);
    if (!failed_)
    {
        over_ = true;
        if (log_) log_("Watching: the match's stream ended");
    }
    changed_.notify_all();
}
} // namespace mscharged::platform
