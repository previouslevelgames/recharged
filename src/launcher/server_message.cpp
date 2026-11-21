#include "launcher/server_message.h"

#include "platform/json.h"

#include <cctype>
#include <exception>
#include <utility>

namespace mscharged::launcher
{
namespace
{
constexpr const char* kPath = "/api/v1/message";
// The server takes at most 2000 characters; in UTF-8 a few times that.
constexpr std::size_t kMostBytes = 8000;

// Days from 1970-01-01 to the civil date (proleptic Gregorian).
std::int64_t DaysFromCivil(std::int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = unsigned(year - era * 400);
    const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + std::int64_t(day_of_era) - 719468;
}
} // namespace

std::optional<std::int64_t> ParseRfc3339(std::string_view text)
{
    std::size_t at = 0;
    const auto digits = [&](std::size_t count) -> std::optional<int> {
        if (at + count > text.size()) return std::nullopt;
        int value = 0;
        for (std::size_t n = 0; n < count; ++n)
        {
            const unsigned char c = static_cast<unsigned char>(text[at + n]);
            if (!std::isdigit(c)) return std::nullopt;
            value = value * 10 + (c - '0');
        }
        at += count;
        return value;
    };
    const auto expect = [&](std::string_view any) {
        if (at >= text.size() || any.find(text[at]) == std::string_view::npos) return false;
        ++at;
        return true;
    };
    const auto year = digits(4);
    if (!year || !expect("-")) return std::nullopt;
    const auto month = digits(2);
    if (!month || !expect("-")) return std::nullopt;
    const auto day = digits(2);
    if (!day || !expect("Tt ")) return std::nullopt;
    const auto hour = digits(2);
    if (!hour || !expect(":")) return std::nullopt;
    const auto minute = digits(2);
    if (!minute || !expect(":")) return std::nullopt;
    const auto second = digits(2);
    if (!second) return std::nullopt;
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 || *second > 60)
        return std::nullopt;
    // Fractions of a second count as the second they are in.
    if (at < text.size() && text[at] == '.')
    {
        ++at;
        const std::size_t first = at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
        if (at == first) return std::nullopt;
    }
    std::int64_t offset = 0;
    if (!expect("Zz"))
    {
        if (at >= text.size() || (text[at] != '+' && text[at] != '-')) return std::nullopt;
        const bool behind = text[at++] == '-';
        const auto hours = digits(2);
        if (!hours || !expect(":")) return std::nullopt;
        const auto minutes = digits(2);
        if (!minutes || *hours > 23 || *minutes > 59) return std::nullopt;
        offset = (std::int64_t(*hours) * 60 + *minutes) * 60 * (behind ? -1 : 1);
    }
    if (at != text.size()) return std::nullopt;
    const std::int64_t days = DaysFromCivil(*year, unsigned(*month), unsigned(*day));
    return days * 86400 + std::int64_t(*hour) * 3600 + std::int64_t(*minute) * 60 + *second - offset;
}

std::optional<ServerMessage> ParseServerMessage(std::string_view body, bool* readable)
{
    if (readable) *readable = false;
    try
    {
        const auto json = platform::ParseJson(body);
        if (!json.IsObject()) return std::nullopt;
        const auto& message = json["message"];
        if (message.IsNull())
        {
            if (readable) *readable = true;
            return std::nullopt;
        }
        if (!message.IsObject() || !message["markdown"].IsString()) return std::nullopt;
        const auto id = message["id"].Integer();
        if (!id || *id <= 0) return std::nullopt;
        ServerMessage result;
        result.id = *id;
        result.markdown = message["markdown"].Text();
        if (result.markdown.size() > kMostBytes) return std::nullopt;
        result.warning = message["tone"].Text() == "warning";
        if (const auto& ends = message["ends_at"]; !ends.IsNull())
        {
            if (!ends.IsString()) return std::nullopt;
            result.ends_at = ParseRfc3339(ends.Text());
            if (!result.ends_at) return std::nullopt;
        }
        if (readable) *readable = true;
        // A message with nothing to show is none.
        if (result.markdown.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
        return result;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

bool ServerMessageCurrent(const ServerMessage& message, std::int64_t now)
{
    return !message.ends_at || now < *message.ends_at;
}

ServerMessageFetch::ServerMessageFetch(std::string server, Log log, Request request)
    : server_(std::move(server)), log_(std::move(log)),
      request_(request ? std::move(request) : Request([](const platform::HttpRequest& http) {
          return platform::HttpGet(http);
      }))
{
    thread_ = std::thread([this] { Run(); });
}

ServerMessageFetch::~ServerMessageFetch()
{
    cancel_->Cancel();
    if (thread_.joinable()) thread_.join();
}

bool ServerMessageFetch::Done(std::optional<ServerMessage>* message) const
{
    std::lock_guard lock(mutex_);
    if (done_ && message) *message = message_;
    return done_;
}

void ServerMessageFetch::Run()
{
    platform::HttpRequest http;
    http.url = server_ + kPath;
    // A test server on this computer or network (ProfileServerAddress allows no other).
    http.allow_http = server_.rfind("http://", 0) == 0;
    http.connect_timeout = 10;
    http.stall_timeout = 15;
    http.max_size = 64u << 10;
    http.cancel = cancel_;
    std::optional<ServerMessage> message;
    std::string line;
    try
    {
        const auto response = request_(http);
        bool readable = false;
        if (response.status == 404 || response.status == 405)
            line = "Server message: none (the server has no messages, HTTP " + std::to_string(response.status) + ")";
        else if (response.status != 200)
            line = "Server message: not available (HTTP " + std::to_string(response.status) + ")";
        else if ((message = ParseServerMessage(response.body, &readable)))
            line = "Server message: " + std::to_string(message->id) + " from " + server_;
        else if (readable)
            line = "Server message: none";
        else
            line = "Server message: not available (an answer it cannot read)";
    }
    catch (const platform::HttpError& error)
    {
        if (error.Cancelled()) return;
        line = std::string("Server message: not available (") + error.what() + ")";
    }
    catch (const std::exception& error)
    {
        line = std::string("Server message: not available (") + error.what() + ")";
    }
    {
        std::lock_guard lock(mutex_);
        message_ = std::move(message);
        done_ = true;
    }
    if (log_) log_(line);
}
} // namespace mscharged::launcher
