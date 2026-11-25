#pragma once

#include "platform/http_client.h"
#include "platform/startgg.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mscharged::platform
{
// Where the TOURNAMENT page's event comes from: start.gg itself, or a saved
// answer on disk (to work on the page without an account or a network).
// Fetch blocks; call it off the game's thread.
class StartggSource
{
public:
    virtual ~StartggSource() = default;
    // The event `slug` with all its sets (every page), or why not.
    virtual StartggAnswer Fetch(std::string_view slug) = 0;
    // From another thread: the Fetch on its way ends now, and any after it
    // (the feed goes).
    virtual void Cancel() {}
};

// start.gg's GraphQL endpoint.
inline constexpr const char* kStartggApi = "https://api.start.gg/gql/alpha";
// The most pages of sets one Fetch asks for (40 sets each).
inline constexpr int kStartggMaxPages = 10;

// An answer saved in `file` (as start.gg sends it; the slug is not read).
std::unique_ptr<StartggSource> StartggFileSource(std::filesystem::path file);

// How a request goes out: HttpGet, or a test's.
using StartggTransport = std::function<HttpResponse(const HttpRequest&)>;
// start.gg with the personal token `token` (its Developer Settings). The
// token goes only into the requests' Authorization header, never into an
// error. `url`: another endpoint (a test server on this computer, http://
// allowed only for localhost and 127.0.0.1).
std::unique_ptr<StartggSource> StartggHttpSource(std::string token, std::string url = kStartggApi,
                                                 StartggTransport transport = {});
} // namespace mscharged::platform
