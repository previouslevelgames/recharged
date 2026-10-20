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
    request.user_agent = "Recharged/" + build.version;
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

void UpdateService::Check(UpdateChannel channel, int chosen)
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
        [channel, chosen](Status& status) {
            status.state = State::Checking;
            status.channel = channel;
            status.error.clear();
            status.offer.reset();
            status.pull.reset();
            status.install_blocker.clear();
            // test_builds and track stay until the check replaces them.
            status.chosen = chosen;
        },
        [this, channel, chosen, cancel] { RunCheck(channel, chosen, cancel); });
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

void UpdateService::RunCheck(UpdateChannel channel, int chosen, std::shared_ptr<platform::HttpCancel> cancel)
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
        std::vector<PublishedBuild> test_builds;
        int track = 0;
        if (channel == UpdateChannel::Test)
        {
            for (const PublishedBuild* test : TestBuilds(build, published)) test_builds.push_back(*test);
            track = FollowedPullRequest(build, chosen, published);
        }
        const PublishedBuild* found = FindUpdate(build, channel, published, chosen);
        if (!found)
        {
            Set([&](Status& status) {
                status.state = State::UpToDate;
                status.checked = std::time(nullptr);
                status.test_builds = std::move(test_builds);
                status.track = track;
            });
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
            status.checked = std::time(nullptr);
            status.test_builds = std::move(test_builds);
            status.track = track;
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

bool UpdateService::Preview(std::string_view state)
{
    BuildIdentity build = Build();
    if (build.package.empty()) build.package = "linux-x86_64";
    const PortVersion self = ParsePortVersion(build.version);
    auto made_up = [&](bool test, int pull_request, PortVersion version, std::string title, std::string notes) {
        PublishedBuild offer;
        offer.test = test;
        offer.pull_request = pull_request;
        offer.version = std::move(version);
        offer.tag = test ? "test-pr" + std::to_string(pull_request) + "-" + offer.version.commit
                         : "v" + VersionNumber(offer.version);
        offer.title = std::move(title);
        offer.notes = std::move(notes);
        offer.page = test ? PullRequestPageUrl(build.repository, pull_request)
                          : ReleasesPageUrl(build.repository) + "/tag/" + offer.tag;
        offer.published = std::time(nullptr) - 3 * 3600;
        PackageAsset asset;
        asset.name = "recharged-" + VersionNumber(offer.version) + "+g" + offer.version.commit + "-" + build.package
            + ".tar.gz";
        asset.url = offer.page;
        asset.size = 19713229;
        offer.assets.push_back(std::move(asset));
        return offer;
    };
    PortVersion next = self.Valid() ? self : ParsePortVersion("1.0.10");
    ++next.patch;
    next.commit = "4f2a9c81d03e";
    const std::string release_notes =
        "Take match day further: this made-up release shows how release notes look in the launcher.\n\n"
        "## Gameplay\n- **Mirror matches (#51):** both teams can pick the same captain in offline Domination.\n"
        "- **Fast terrain (#43):** every stadium plays like the dry field of Bowser Stadium.\n\n"
        "## Launcher\n- Updates install in place and restart the launcher; settings and saves stay.\n"
        "- The Play page switches between releases and test builds.";
    const std::string pull_notes =
        "Adds the Release / Test switch and in-place updates to the launcher.\n\n"
        "## To try\n- Switch to **Test** on the Play page and install this build.\n"
        "- Switch back to **Release** to return to the official release.";
    Status status;
    status.channel = UpdateChannel::Release;
    status.checked = std::time(nullptr) - 120;
    if (state == "uptodate") status.state = State::UpToDate;
    else if (state == "notest")
    {
        status.state = State::UpToDate;
        status.channel = UpdateChannel::Test;
    }
    else if (state == "checking") status.state = State::Checking;
    else if (state == "failed")
    {
        status.state = State::CheckFailed;
        status.error = "Could not resolve host: api.github.com";
    }
    else if (state == "available" || state == "blocked" || state == "downloading" || state == "installing"
             || state == "installfailed")
    {
        status.offer = made_up(false, 0, next, VersionNumber(next) + " \xE2\x80\x93 Mirror matches and fast terrain",
                               release_notes);
        status.state = state == "downloading" ? State::Downloading
            : state == "installing"           ? State::Installing
            : state == "installfailed"        ? State::InstallFailed
                                              : State::Available;
        if (state == "blocked")
            status.install_blocker = "This build was not installed from a package (no SOURCE-MANIFEST.json).";
        if (state == "installfailed") status.error = "The download is damaged (its checksum does not match).";
        status.received = 8598323;
        status.total = 19713229;
    }
    else if (state == "test" || state == "newtest")
    {
        PortVersion version = next;
        version.commit = "b81e07c4a2d9";
        status.channel = UpdateChannel::Test;
        status.offer = made_up(true, 52, version, "", pull_notes);
        PullRequestInfo pull;
        pull.number = 52;
        pull.title = "Launcher: switch between releases and test builds";
        pull.body = pull_notes;
        pull.page = status.offer->page;
        pull.open = true;
        status.pull = pull;
        status.state = State::Available;
        if (state == "newtest")
        {
            build.origin = BuildOrigin::Test;
            build.pull_request = 52;
            build.version = VersionNumber(version) + "+g0c3d5e6f7a8b";
            build.built = status.offer->published - 86400;
        }
    }
    else if (state == "back")
    {
        build.origin = BuildOrigin::Test;
        build.pull_request = 48;
        PortVersion older = self.Valid() ? self : ParsePortVersion("1.0.10");
        older.patch = std::max(0, older.patch - 1);
        older.commit = "6144eb682311";
        status.offer = made_up(false, 0, older, VersionNumber(older) + " \xE2\x80\x93 The current release",
                               release_notes);
        status.state = State::Available;
    }
    else if (state == "choose" || state == "tracks" || state == "switch")
    {
        // Three release pull requests open at once, each with its test build.
        struct Track
        {
            int pull_request;
            const char *version, *commit, *title;
            int hours_ago;
        };
        const Track tracks[] = {
            {74, "1.2.0", "e4b1c0d9a7f3", "Release 1.2.0 \xE2\x80\x93 Mods", 50},
            {72, "1.1.0", "9a3f5b7c1e2d", "Release 1.1.0 \xE2\x80\x93 NetCode!", 2},
            {69, "1.0.11", "4d5a65b360a5", "Release 1.0.11 \xE2\x80\x93 Wii Remote speaker, gyro pointer and a HUD fix", 26},
        };
        for (const auto& track : tracks)
        {
            PortVersion version = ParsePortVersion(track.version);
            version.commit = track.commit;
            PublishedBuild offer = made_up(true, track.pull_request, version,
                                           std::string("Test build: ") + track.title + " (#"
                                               + std::to_string(track.pull_request) + ")",
                                           pull_notes);
            offer.published = std::time(nullptr) - std::int64_t(track.hours_ago) * 3600;
            status.test_builds.push_back(std::move(offer));
        }
        status.channel = UpdateChannel::Test;
        status.state = State::UpToDate;
        if (state == "choose")
        {
            build.origin = BuildOrigin::Release;
            build.version = "1.0.10+gc215f76f1c91";
        }
        else
        {
            build.origin = BuildOrigin::Test;
            build.pull_request = 69;
            build.version = "1.0.11+g4d5a65b360a5";
            build.built = status.test_builds[2].published;
            status.track = 69;
        }
        if (state == "switch")
        {
            const std::string netcode_notes =
                "Version 1.1.0 brings online matches against other Recharged players.\n\n"
                "## Online\n- **Online play (#2):** Domination matches over the port's own service.\n\n"
                "## To try\n- Choose **Online** in the main menu and invite a friend.";
            status.chosen = status.track = 72;
            status.offer = status.test_builds[1];
            status.offer->notes = netcode_notes;
            PullRequestInfo pull;
            pull.number = 72;
            pull.title = "Release 1.1.0 \xE2\x80\x93 NetCode!";
            pull.body = netcode_notes;
            pull.page = status.offer->page;
            pull.open = true;
            status.pull = pull;
            status.state = State::Available;
        }
    }
    else return false;
    std::lock_guard<std::mutex> lock(mutex_);
    build_ = std::move(build);
    status.serial = status_.serial + 1;
    status_ = std::move(status);
    return true;
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
