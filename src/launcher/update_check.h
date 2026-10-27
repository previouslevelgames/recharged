#pragma once

#include "platform/json.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What the launcher's update check decides, without network or UI: the
// running build's origin, GitHub's releases and test builds, and which of
// them is an update. Test builds are prereleases tagged test-pr<N>-<commit>
// (.github/workflows/build.yml); packages are named
// recharged-<version>-<platform>-<arch>.zip/.tar.gz (tools/package_build.py),
// mscharged-... before the rename (#64).
namespace mscharged::launcher
{
// A port version, x.y.z, and the commit the build adds as "+g<commit>".
struct PortVersion
{
    int major = -1, minor = 0, patch = 0;
    std::string commit;
    bool Valid() const { return major >= 0; }
};
// "1.0.10", "v1.0.10" or "1.0.10+g7861e603a8ec" (a trailing ".dirty" is ignored).
PortVersion ParsePortVersion(std::string_view text);
// Orders x.y.z; the commit does not count.
int CompareVersions(const PortVersion& a, const PortVersion& b);
std::string VersionNumber(const PortVersion& version); // "1.0.10"

enum class BuildOrigin { Release, Test, Dev };

// The running build: where it comes from and which packages fit it.
struct BuildIdentity
{
    std::string version; // e.g. 1.0.10+geb8476aad265
    BuildOrigin origin = BuildOrigin::Dev;
    std::string repository; // owner/name
    std::string commit;
    int pull_request = 0;     // test builds
    std::int64_t run = 0;     // CI run, 0 for none
    std::int64_t built = 0;   // build time, seconds since 1970 (0: unknown)
    std::int64_t commit_time = 0;
    // linux-x86_64, steamdeck-x86_64, windows-x86_64, macos-universal, macos-arm64, macos-x86_64
    std::string package;
    // When the build was made: the build time, else its commit's time.
    std::int64_t Time() const { return built ? built : commit_time; }
};

// "Official release", "Test build of pull request #48", "Local build", ...
std::string DescribeOrigin(const BuildIdentity& build);
// One line with origin, commit and time, for --version and the session log.
std::string DescribeBuild(const BuildIdentity& build);

enum class UpdateChannel { Release, Test };
// launcher.update_channel: release, test, or auto, where test builds follow
// test builds and every other build follows releases.
UpdateChannel ResolveChannel(const std::string& setting, const BuildIdentity& build);

struct PackageAsset
{
    std::string name, url;
    std::string digest; // "sha256:<hex>" when GitHub reports it
    std::uint64_t size = 0;
};

// An official release, or the test build of a pull request.
struct PublishedBuild
{
    std::string tag, title, page, notes;
    bool test = false;
    int pull_request = 0; // test builds
    // A test build its pull request keeps out of the launcher: the notes (the
    // pull request's description, copied by CI) have the line kHideTestBuilds.
    bool hidden = false;
    PortVersion version;
    std::int64_t published = 0;
    std::vector<PackageAsset> assets;
    // The package for a package id (e.g. windows-x86_64), or null. A Mac takes
    // the universal package when there is one.
    const PackageAsset* PackageFor(const std::string& package) const;
    const PackageAsset* Checksums() const; // SHA256SUMS, or null
};

// GitHub's release list (GET /repos/{owner}/{name}/releases): official
// releases and test builds; drafts, other prereleases and entries without a
// recognizable version are left out.
std::vector<PublishedBuild> ParseReleases(const platform::JsonValue& releases);

// The line in a pull request's description that keeps its test builds out of
// the launcher (an HTML comment: GitHub does not show it).
inline constexpr std::string_view kHideTestBuilds = "<!-- launcher: hide -->";

// The builds the launcher shows: a hidden test build only to a test build of
// the same pull request (its updates) or when launcher.test_build chose that
// pull request (`chosen`).
std::vector<PublishedBuild> ShownBuilds(const BuildIdentity& build, std::vector<PublishedBuild> published, int chosen);

struct PullRequestInfo
{
    int number = 0;
    std::string title, body, page;
    bool open = false, merged = false;
};
PullRequestInfo ParsePullRequest(const platform::JsonValue& pull);

// The newest test build of each pull request that has a package for this
// build: highest version first, then the latest published. Several release
// pull requests can be open at once (say 1.0.11 and 1.1.0), each with its
// own test builds.
std::vector<const PublishedBuild*> TestBuilds(const BuildIdentity& build,
                                              const std::vector<PublishedBuild>& published);

// The pull request whose test builds the test channel follows: `chosen`
// (launcher.test_build) while it has test builds, else the running test
// build's own while that has any; 0 for none.
int FollowedPullRequest(const BuildIdentity& build, int chosen, const std::vector<PublishedBuild>& published);

// The build the channel offers, with a package for this build, or null.
// Release channel: the newest release when it is newer; a test build is
// offered it even when older, the way back from test builds. Test channel:
// - following a test build's own pull request, the newest of its test builds
//   and the releases when newer (so a tested version moves on to its release);
// - following another pull request (chosen), its newest test build, whatever
//   its version: a switch;
// - following none, a newer release; a build that never followed a test
//   build is also offered the test build of the only open pull request.
//   Another pull request's test build is never offered by itself.
// A build is newer with a higher version or, for release and test builds, a
// later build of the same version. Local builds are only offered higher
// versions; the very same build (same commit) is never offered.
const PublishedBuild* FindUpdate(const BuildIdentity& build, UpdateChannel channel,
                                 const std::vector<PublishedBuild>& published, int chosen = 0);

// Whether `offer` is newer than the running build, by FindUpdate's rule.
bool IsNewer(const BuildIdentity& build, const PublishedBuild& offer);

// What an offer means for the running build.
enum class OfferKind
{
    Update,        // a newer release, or a newer test build of the same pull request
    TestBuild,     // a test build for another kind of build, or of another pull request
    BackToRelease, // a test build's way back to the newest release, which is not newer
};
OfferKind ClassifyOffer(const BuildIdentity& build, const PublishedBuild& offer);

// Whether a version installs its updates itself (they arrived with 1.0.10).
bool UpdatesItself(const PortVersion& version);

// The notice after an update replaced the build of version `replaced`, e.g.
// "Updated from 1.0.9 to release 1.0.10.".
std::string DescribeReplacement(std::string_view replaced, const BuildIdentity& build);

// The expected SHA-256 (lowercase hex) of an asset: GitHub's digest, else its
// line in the release's SHA256SUMS; empty when neither knows it.
std::string ExpectedSha256(const PackageAsset& asset, std::string_view checksums);

// Seconds since 1970 for an ISO 8601 UTC time ("2026-10-15T17:22:00Z"); 0 if malformed.
std::int64_t ParseUtcTime(std::string_view text);
std::string FormatUtcTime(std::int64_t seconds); // "2026-10-15 17:22 UTC"

std::string ReleasesApiUrl(const std::string& repository);
std::string PullRequestApiUrl(const std::string& repository, int number);
std::string ReleasesPageUrl(const std::string& repository);
std::string PullRequestPageUrl(const std::string& repository, int number);
// The notes of the running build: its release, its pull request, or the
// releases page for other builds.
std::string BuildNotesUrl(const BuildIdentity& build);
} // namespace mscharged::launcher
