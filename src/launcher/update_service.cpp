#include "launcher/update_service.h"

#include "launcher/update_install.h"
#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/path.h"
#include "platform/sha256.h"

#include <chrono>
#include <ctime>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mscharged::launcher
{
namespace
{
namespace fs = std::filesystem;

platform::HttpRequest GitHubRequest(const std::string& url, const BuildIdentity& build,
                                    const std::shared_ptr<platform::HttpCancel>& cancel)
{
    platform::HttpRequest request;
    request.url = url;
    request.user_agent = "mscharged/" + build.version;
    request.headers = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"};
    request.cancel = cancel;
    return request;
}

std::string GitHubFailure(long status)
{
    if (status == 403 || status == 429) return "GitHub is limiting requests from this network; try again later.";
    if (status == 404) return "The repository was not found on GitHub.";
    return "GitHub answered with error " + std::to_string(status) + ".";
}

std::string Stamp()
{
    static unsigned counter = 0;
    return std::to_string(std::time(nullptr)) + "-" + std::to_string(++counter);
}

// Removes `folder` when it holds no files (only empty folders).
void RemoveIfEmpty(const fs::path& folder)
{
    std::error_code error;
    for (const auto& entry : fs::recursive_directory_iterator(folder, error))
        if (!entry.is_directory()) return;
    fs::remove_all(folder, error);
}
} // namespace

UpdateService::UpdateService(BuildIdentity build, fs::path base) : build_(std::move(build)), base_(std::move(base)) {}

UpdateService::~UpdateService()
{
    Cancel();
    if (worker_.joinable()) worker_.join();
}

void UpdateService::Set(const std::function<void(Status&)>& change)
{
    std::lock_guard<std::mutex> lock(mutex_);
    change(status_);
    ++status_.serial;
}

UpdateService::Status UpdateService::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

BuildIdentity UpdateService::Build() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return build_;
}

void UpdateService::Cancel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel_) cancel_->Cancel();
}

void UpdateService::Start(std::shared_ptr<platform::HttpCancel> cancel, const std::function<void(Status&)>& begin,
                          std::function<void()> task)
{
    std::shared_ptr<platform::HttpCancel> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = std::exchange(cancel_, std::move(cancel));
    }
    if (previous) previous->Cancel();
    if (worker_.joinable()) worker_.join();
    // The new state shows at once, before the next Snapshot().
    Set(begin);
    worker_ = std::thread(std::move(task));
}

void UpdateService::Check(UpdateChannel channel)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // An installation is never interrupted.
        if (status_.state == State::Downloading || status_.state == State::Installing
            || status_.state == State::Installed)
            return;
    }
    auto cancel = std::make_shared<platform::HttpCancel>();
    Start(
        cancel,
        [channel](Status& status) {
            status.state = State::Checking;
            status.channel = channel;
            status.error.clear();
            status.offer.reset();
            status.pull.reset();
            status.install_blocker.clear();
        },
        [this, channel, cancel] { RunCheck(channel, cancel); });
}

void UpdateService::Install()
{
    std::optional<PublishedBuild> offer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((status_.state != State::Available && status_.state != State::InstallFailed) || !status_.offer
            || !status_.install_blocker.empty())
            return;
        offer = status_.offer;
    }
    auto cancel = std::make_shared<platform::HttpCancel>();
    Start(
        cancel,
        [](Status& status) {
            status.state = State::Downloading;
            status.error.clear();
            status.received = status.total = 0;
        },
        [this, offer = *offer, cancel] { RunInstall(offer, cancel); });
}

void UpdateService::RunCheck(UpdateChannel channel, std::shared_ptr<platform::HttpCancel> cancel)
{
    try
    {
        BuildIdentity build = Build();
        // The installed package names its platform (e.g. the Steam Deck build of Linux).
        try
        {
            if (fs::exists(base_ / kPackageManifest))
                build.package = ReadPackageManifest(base_ / kPackageManifest).Package();
        }
        catch (const std::exception&) {}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            build_.package = build.package;
        }
        const auto answer = platform::HttpGet(GitHubRequest(ReleasesApiUrl(build.repository), build, cancel));
        if (answer.status != 200) throw std::runtime_error(GitHubFailure(answer.status));
        const auto published = ParseReleases(platform::ParseJson(answer.body));
        const PublishedBuild* found = FindUpdate(build, channel, published);
        if (!found)
        {
            Set([](Status& status) { status.state = State::UpToDate; });
            return;
        }
        PublishedBuild offer = *found;
        // A test build's notes are its pull request's description.
        std::optional<PullRequestInfo> pull;
        if (offer.test)
        {
            try
            {
                const auto reply =
                    platform::HttpGet(GitHubRequest(PullRequestApiUrl(build.repository, offer.pull_request), build, cancel));
                if (reply.status == 200) pull = ParsePullRequest(platform::ParseJson(reply.body));
            }
            catch (const platform::HttpError& error)
            {
                if (error.Cancelled()) throw;
            }
            catch (const std::exception&) {}
        }
        const std::string blocker = InstallBlocker(base_);
        Set([&](Status& status) {
            status.state = State::Available;
            status.offer = std::move(offer);
            status.pull = std::move(pull);
            status.install_blocker = blocker;
        });
    }
    catch (const platform::HttpError& error)
    {
        if (error.Cancelled()) Set([](Status& status) { status.state = State::Idle; });
        else Set([&](Status& status) { status.state = State::CheckFailed; status.error = error.what(); });
    }
    catch (const std::exception& error)
    {
        Set([&](Status& status) { status.state = State::CheckFailed; status.error = error.what(); });
    }
}

void UpdateService::RunInstall(PublishedBuild offer, std::shared_ptr<platform::HttpCancel> cancel)
{
    const fs::path folder = UpdateFolder(base_);
    const std::string stamp = Stamp();
    const fs::path download = folder / ("download-" + stamp), staged = folder / ("staged-" + stamp);
    const fs::path backup = folder / ("previous-" + stamp);
    auto cleanup = [&] {
        std::error_code error;
        fs::remove_all(download, error);
        fs::remove_all(staged, error);
    };
    try
    {
        const BuildIdentity build = Build();
        const PackageAsset* asset = offer.PackageFor(build.package);
        if (!asset) throw std::runtime_error("This release has no package for " + build.package + ".");
        std::string expected = ExpectedSha256(*asset, {});
        if (expected.empty())
            if (const PackageAsset* sums = offer.Checksums())
            {
                const auto reply = platform::HttpGet(GitHubRequest(sums->url, build, cancel));
                if (reply.status == 200) expected = ExpectedSha256(*asset, reply.body);
            }
        if (expected.empty()) throw std::runtime_error("The release has no checksum for " + asset->name + ".");

        fs::create_directories(download);
        const fs::path archive = download / PathFromUtf8(asset->name);
        auto request = GitHubRequest(asset->url, build, cancel);
        request.headers = {"Accept: application/octet-stream"};
        request.stall_timeout = 60;
        const long status = platform::HttpDownload(request, archive, [&](std::uint64_t received, std::uint64_t total) {
            Set([&](Status& state) {
                state.received = received;
                state.total = total ? total : asset->size;
            });
        });
        if (status != 200) throw std::runtime_error("The download failed with error " + std::to_string(status) + ".");
        if (asset->size && fs::file_size(archive) != asset->size)
            throw std::runtime_error("The download is incomplete.");
        if (platform::Sha256File(archive) != expected)
            throw std::runtime_error("The download is damaged (its checksum does not match).");

        Set([](Status& state) { state.state = State::Installing; });
        const fs::path root = UnpackPackage(archive, staged, [&] { return cancel->Cancelled(); });
        const auto incoming = ReadPackageManifest(root / kPackageManifest);
        const PortVersion version = ParsePortVersion(incoming.version);
        if (CompareVersions(version, offer.version) != 0 || version.commit != offer.version.commit)
            throw std::runtime_error("The package holds version " + incoming.version + ", not the one offered.");
        if (cancel->Cancelled()) throw platform::HttpError("Cancelled", true);
        InstallStagedPackage(base_, root, backup);
        cleanup();
        Set([](Status& state) { state.state = State::Installed; });
    }
    catch (const std::exception& error)
    {
        cleanup();
        RemoveIfEmpty(backup);
        const auto* http = dynamic_cast<const platform::HttpError*>(&error);
        const bool cancelled = cancel->Cancelled() || (http && http->Cancelled());
        Set([&](Status& state) {
            state.state = cancelled ? State::Available : State::InstallFailed;
            state.error = cancelled ? std::string() : std::string(error.what());
        });
    }
}
} // namespace mscharged::launcher
