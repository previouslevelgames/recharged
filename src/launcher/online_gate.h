#pragma once

#include "launcher/update_check.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mscharged::platform
{
class HttpCancel;
}

// Enforced updates: an official release plays online only while it is the
// newest release, so players of different versions, whose games can go out of
// sync, do not meet. The launcher installs a newer release by itself; when
// that fails, or the game was started without it, the game's main menu keeps
// Online closed. Test builds and local builds are never held back.
namespace mscharged::launcher
{
// The game module's view of it (charged_configure_online_gate): the same
// numbers on both sides.
enum class OnlineGateState
{
    Open = 0,      // online play allowed
    Checking = 1,  // the check is still running
    Outdated = 2,  // a newer release is out
    Unchecked = 3, // the check failed: no answer, no network, GitHub's limit
};

// Whether this build installs an update of its own line by itself (the
// launcher's enforced updates): releases and test builds. Local builds keep
// the choice.
bool EnforcesUpdates(const BuildIdentity& build);

// Whether this build's online play waits for updates: official releases.
bool GatesOnline(const BuildIdentity& build);

// How long a successful check of the launcher vouches for the game it starts.
inline constexpr std::int64_t kOnlineVerifiedSeconds = 3600;

// Whether the launcher's check at `verified` (seconds since 1970, 0 for none)
// still vouches at `now`.
bool StillVerified(std::int64_t verified, std::int64_t now);

// A release build's verdict from GitHub's release list: Outdated when a newer
// release has a package for it (FindUpdate's rule), with its version in `newer`.
OnlineGateState JudgeOnline(const BuildIdentity& build, const std::vector<PublishedBuild>& published,
                            std::string* newer = nullptr);

// The game's own check, once per start, on a background thread. `report`
// gets each state with a line for the session log: at once (Open, or
// Checking), then the verdict from the worker thread. The destructor stops a
// running check.
class OnlineGate
{
public:
    using Report = std::function<void(OnlineGateState state, const std::string& line)>;

    // `base`: the program's folder (its installed package names the build's
    // platform). `verified`: when the launcher that started the game found
    // this build up to date, 0 when it did not.
    OnlineGate(BuildIdentity build, std::filesystem::path base, std::int64_t verified, Report report);
    ~OnlineGate();
    OnlineGate(const OnlineGate&) = delete;
    OnlineGate& operator=(const OnlineGate&) = delete;

    OnlineGateState State() const { return state_.load(); }

private:
    void Run(BuildIdentity build, std::filesystem::path base);
    void Settle(OnlineGateState state, const std::string& line);

    Report report_;
    std::atomic<OnlineGateState> state_{OnlineGateState::Open};
    std::shared_ptr<platform::HttpCancel> cancel_;
    std::thread worker_;
};
} // namespace mscharged::launcher
