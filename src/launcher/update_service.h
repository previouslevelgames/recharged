#pragma once

#include "launcher/update_check.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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
        std::string install_blocker;         // why Update would only open the download page
        std::uint64_t received = 0, total = 0;
        std::uint64_t serial = 0; // changes with every state change
    };

    // `base`: the game folder (the executable's directory).
    UpdateService(BuildIdentity build, std::filesystem::path base);
    ~UpdateService();
    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    void Check(UpdateChannel channel);
    void Install(); // the offer of the last check
    void Cancel();  // a running check or download
    Status Snapshot() const;
    // The running build; its package id comes from the installed manifest once a check ran.
    BuildIdentity Build() const;

private:
    void Start(std::shared_ptr<platform::HttpCancel> cancel, const std::function<void(Status&)>& begin,
               std::function<void()> task);
    void Set(const std::function<void(Status&)>& change);
    void RunCheck(UpdateChannel channel, std::shared_ptr<platform::HttpCancel> cancel);
    void RunInstall(PublishedBuild offer, std::shared_ptr<platform::HttpCancel> cancel);

    BuildIdentity build_;
    const std::filesystem::path base_;
    mutable std::mutex mutex_;
    Status status_;
    std::shared_ptr<platform::HttpCancel> cancel_;
    std::thread worker_;
};
} // namespace mscharged::launcher
