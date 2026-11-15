#include "platform/startgg_source.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace mscharged::platform
{
namespace
{
class FileSource final : public StartggSource
{
public:
    explicit FileSource(std::filesystem::path file) : file_(std::move(file)) {}

    StartggAnswer Fetch(std::string_view) override
    {
        std::ifstream in(file_, std::ios::binary);
        if (!in)
        {
            StartggAnswer answer;
            answer.error = "start.gg file not found: " + file_.filename().string();
            return answer;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return ReadStartggAnswer(200, text.str());
    }

private:
    std::filesystem::path file_;
};

bool LocalHttp(const std::string& url)
{
    for (const char* prefix : {"http://localhost:", "http://localhost/", "http://127.0.0.1:", "http://127.0.0.1/"})
        if (url.rfind(prefix, 0) == 0) return true;
    return false;
}

class HttpSource final : public StartggSource
{
public:
    HttpSource(std::string token, std::string url, StartggTransport transport)
        : token_(std::move(token)), url_(std::move(url)), transport_(std::move(transport))
    {
        if (!transport_) transport_ = [](const HttpRequest& request) { return HttpGet(request); };
    }

    StartggAnswer Fetch(std::string_view slug) override
    {
        StartggAnswer first;
        for (int page = 1; page <= kStartggMaxPages; ++page)
        {
            StartggAnswer answer = Page(slug, page);
            if (!answer.event) return answer; // a page missing: no event half read
            if (page == 1) first = std::move(answer);
            else AddStartggPage(*first.event, std::move(*answer.event));
            if (page >= first.event->total_pages) break;
        }
        return first;
    }

private:
    StartggAnswer Page(std::string_view slug, int page)
    {
        StartggAnswer answer;
        if (token_.empty())
        {
            answer.error = "no start.gg token ([startgg] dev_token)";
            return answer;
        }
        if (url_.rfind("https://", 0) != 0 && !LocalHttp(url_))
        {
            answer.error = "start.gg's address must be https";
            return answer;
        }
        HttpRequest request;
        request.url = url_;
        request.method = "POST";
        request.allow_http = LocalHttp(url_);
        request.body = StartggEventRequest(slug, page);
        request.headers = {"Content-Type: application/json", "Authorization: Bearer " + token_};
        request.max_size = 4u << 20;
        try
        {
            const HttpResponse response = transport_(request);
            return ReadStartggAnswer(response.status, response.body);
        }
        catch (const HttpError& error)
        {
            // (The error names the transfer's failure, not the request.)
            answer.error = std::string("start.gg could not be reached: ") + error.what();
            return answer;
        }
    }

    std::string token_, url_;
    StartggTransport transport_;
};
} // namespace

std::unique_ptr<StartggSource> StartggFileSource(std::filesystem::path file)
{
    return std::make_unique<FileSource>(std::move(file));
}

std::unique_ptr<StartggSource> StartggHttpSource(std::string token, std::string url, StartggTransport transport)
{
    return std::make_unique<HttpSource>(std::move(token), std::move(url), std::move(transport));
}
} // namespace mscharged::platform
