#pragma once

#include "launcher/update_check.h"
#include "platform/http_client.h"

#include <memory>
#include <string>

// The requests the update checks send to GitHub's API, and what a failed
// answer means: shared by the launcher's updates and the game's online check.
namespace mscharged::launcher
{
inline platform::HttpRequest GitHubRequest(const std::string& url, const BuildIdentity& build,
                                           const std::shared_ptr<platform::HttpCancel>& cancel)
{
    platform::HttpRequest request;
    request.url = url;
    request.user_agent = "Recharged/" + build.version;
    request.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"};
    request.cancel = cancel;
    return request;
}

inline std::string GitHubFailure(long status)
{
    if (status == 403 || status == 429) return "GitHub is limiting requests from this network; try again later.";
    if (status == 404) return "The repository was not found on GitHub.";
    return "GitHub answered with error " + std::to_string(status) + ".";
}
} // namespace mscharged::launcher
