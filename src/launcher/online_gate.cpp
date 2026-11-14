#include "launcher/online_gate.h"

#include "launcher/github_requests.h"
#include "launcher/update_install.h"
#include "platform/http_client.h"
#include "platform/json.h"

#include <ctime>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mscharged::launcher
{
bool EnforcesUpdates(const BuildIdentity& build) { return build.origin != BuildOrigin::Dev; }

bool GatesOnline(const BuildIdentity& build) { return build.origin == BuildOrigin::Release; }

bool StillVerified(std::int64_t verified, std::int64_t now)
{
    return verified > 0 && now >= verified && now - verified < kOnlineVerifiedSeconds;
}

OnlineGateState JudgeOnline(const BuildIdentity& build, const std::vector<PublishedBuild>& published,
                            std::string* newer)
{
    const PublishedBuild* update = FindUpdate(build, UpdateChannel::Release, published);
    if (!update) return OnlineGateState::Open;
    if (newer) *newer = VersionNumber(update->version);
    return OnlineGateState::Outdated;
}

OnlineGate::OnlineGate(BuildIdentity build, std::filesystem::path base, std::int64_t verified, Report report)
    : report_(std::move(report))
{
    if (!GatesOnline(build))
    {
        Settle(OnlineGateState::Open, "Online: on (only releases wait for updates)");
        return;
    }
    if (StillVerified(verified, std::time(nullptr)))
    {
        Settle(OnlineGateState::Open, "Online: on (the launcher found this release up to date)");
        return;
    }
    Settle(OnlineGateState::Checking, "Online: checking for a newer release");
    cancel_ = std::make_shared<platform::HttpCancel>();
    worker_ = std::thread([this, build = std::move(build), base = std::move(base)]() mutable {
        Run(std::move(build), std::move(base));
    });
}

OnlineGate::~OnlineGate()
{
    if (cancel_) cancel_->Cancel();
    if (worker_.joinable()) worker_.join();
}

void OnlineGate::Settle(OnlineGateState state, const std::string& line)
{
    state_ = state;
    if (report_) report_(state, line);
}

void OnlineGate::Run(BuildIdentity build, std::filesystem::path base)
{
    try
    {
        // The installed package names the platform (the Steam Deck build of Linux).
        try
        {
            std::error_code error;
            if (std::filesystem::exists(base / kPackageManifest, error))
                build.package = ReadPackageManifest(base / kPackageManifest).Package();
        }
        catch (const std::exception&) {}
        if (const std::string missing = platform::HttpUnavailable(); !missing.empty())
            throw std::runtime_error(missing);
        const auto answer = platform::HttpGet(GitHubRequest(ReleasesApiUrl(build.repository), build, cancel_));
        if (answer.status != 200) throw std::runtime_error(GitHubFailure(answer.status));
        const auto published = ParseReleases(platform::ParseJson(answer.body));
        std::string newer;
        if (JudgeOnline(build, published, &newer) == OnlineGateState::Outdated)
            Settle(OnlineGateState::Outdated,
                   "Online: off until Recharged is updated (release " + newer + " is out)");
        else Settle(OnlineGateState::Open, "Online: on (this is the newest release)");
    }
    catch (const platform::HttpError& error)
    {
        if (error.Cancelled()) return;
        Settle(OnlineGateState::Unchecked,
               std::string("Online: off, the update check failed (") + error.what() + ")");
    }
    catch (const std::exception& error)
    {
        Settle(OnlineGateState::Unchecked,
               std::string("Online: off, the update check failed (") + error.what() + ")");
    }
}
} // namespace mscharged::launcher
