#pragma once

#include "launcher/update_check.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mscharged::platform
{
class HttpCancel;
}

namespace mscharged::launcher
{
// The launcher's update check and installation, on a background thread.
// The launcher polls Snapshot() every frame. One task runs at a time: a new
// check stops a running check; nothing interrupts an installation's file moves.
class UpdateService
{
public:
    enum class State { Idle, Checking, UpToDate, Available, CheckFailed, Downloading, Installing, Installed, InstallFailed };

    struct Status
    {
        State state = State::Idle;
        UpdateChannel channel = UpdateChannel::Release;
        std::string error; // CheckFailed, InstallFailed
        std::optional<PublishedBuild> offer;
        std::optional<PullRequestInfo> pull; // the pull request of a test build offer, when known
        // Test channel: the newest test build of each open pull request
        // (TestBuilds) and the pull request followed (0: none), from the last
        // completed check, and the one chosen.
        std::vector<PublishedBuild> test_builds;
        int track = 0, chosen = 0;
        std::string install_blocker;         // why Update would only open the download page
        std::uint64_t received = 0, total = 0;
        std::int64_t checked = 0; // when the last check succeeded, seconds since 1970
        std::uint64_t serial = 0; // changes with every state change
    };

    // `base`: the game folder (the executable's directory).
    UpdateService(BuildIdentity build, std::filesystem::path base);
    ~UpdateService();
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    // `chosen`: the pull request whose test builds the test channel follows
    // (launcher.test_build), 0 for the running test build's own.
    // `hidden`: also the test builds their pull request hides (ShownBuilds).
    void Check(UpdateChannel channel, int chosen = 0, bool hidden = false);
    void Install(); // the offer of the last check
    void Cancel();  // a running check or download
    Status Snapshot() const;
    // The running build; its package id comes from the installed manifest once a check ran.
    BuildIdentity Build() const;
    // Development captures: shows a made-up state without network ("uptodate",
    // "notest", "checking", "failed", "available", "newtest", "test", "back",
    // "blocked", "downloading", "installing", "installfailed", and with
    // several open pull requests "choose", "tracks", "switch"). False for others.
    bool Preview(std::string_view state);

private:
    void Start(std::shared_ptr<platform::HttpCancel> cancel, const std::function<void(Status&)>& begin,
               std::function<void()> task);
    void Set(const std::function<void(Status&)>& change);
    void RunCheck(UpdateChannel channel, int chosen, bool hidden, std::shared_ptr<platform::HttpCancel> cancel);
    void RunInstall(PublishedBuild offer, std::shared_ptr<platform::HttpCancel> cancel);

    BuildIdentity build_;
    const std::filesystem::path base_;
    mutable std::mutex mutex_;
    Status status_;
    std::shared_ptr<platform::HttpCancel> cancel_;
    std::thread worker_;
};
} // namespace mscharged::launcher
