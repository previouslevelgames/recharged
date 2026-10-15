#include "launcher/update_check.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace mscharged::launcher
{
namespace
{
constexpr std::string_view kPlatforms[] = {"windows", "linux", "steamdeck", "macos"};

bool ParseNumber(std::string_view text, int& value)
{
    if (text.empty() || text.size() > 9) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool IsHex(std::string_view text)
{
    return !text.empty()
        && std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

// "test-pr48-eb8476aad265": the pull request and the commit.
bool ParseTestTag(std::string_view tag, int& pull_request, std::string& commit)
{
    if (tag.substr(0, 7) != "test-pr") return false;
    tag.remove_prefix(7);
    const auto dash = tag.find('-');
    if (dash == std::string_view::npos || !ParseNumber(tag.substr(0, dash), pull_request) || pull_request <= 0)
        return false;
    commit = std::string(tag.substr(dash + 1));
    return IsHex(commit);
}

// mscharged-<version>-<platform>-<arch>.zip/.tar.gz: the version text.
bool PackageVersion(std::string_view name, std::string& version)
{
    if (name.substr(0, 10) != "mscharged-") return false;
    name.remove_prefix(10);
    if (name.size() > 4 && name.substr(name.size() - 4) == ".zip") name.remove_suffix(4);
    else if (name.size() > 7 && name.substr(name.size() - 7) == ".tar.gz") name.remove_suffix(7);
    else return false;
    for (const auto platform : kPlatforms)
    {
        const std::string marker = "-" + std::string(platform) + "-";
        const auto at = name.rfind(marker);
        if (at != std::string_view::npos && at > 0 && at + marker.size() < name.size())
        {
            version = std::string(name.substr(0, at));
            return true;
        }
    }
    return false;
}

bool Newer(const PublishedBuild& a, const PublishedBuild& b)
{
    const int order = CompareVersions(a.version, b.version);
    return order > 0 || (order == 0 && a.published > b.published);
}

std::int64_t DaysFromCivil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return std::int64_t(era) * 146097 + doe - 719468;
}

std::string Short(const std::string& commit) { return commit.substr(0, 12); }
} // namespace

PortVersion ParsePortVersion(std::string_view text)
{
    PortVersion version;
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    std::string_view build;
    if (const auto plus = text.find('+'); plus != std::string_view::npos)
    {
        build = text.substr(plus + 1);
        text = text.substr(0, plus);
    }
    int parts[3] = {};
    for (int n = 0; n < 3; ++n)
    {
        const auto dot = text.find('.');
        if ((n < 2) == (dot == std::string_view::npos)) return {};
        if (!ParseNumber(text.substr(0, dot), parts[n]) || parts[n] < 0) return {};
        text = n < 2 ? text.substr(dot + 1) : std::string_view();
    }
    if (!build.empty())
    {
        if (build.size() > 6 && build.substr(build.size() - 6) == ".dirty") build.remove_suffix(6);
        if (build.size() < 2 || build[0] != 'g' || !IsHex(build.substr(1))) return {};
        version.commit = std::string(build.substr(1));
    }
    version.major = parts[0];
    version.minor = parts[1];
    version.patch = parts[2];
    return version;
}

int CompareVersions(const PortVersion& a, const PortVersion& b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::string VersionNumber(const PortVersion& version)
{
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
}

std::string DescribeOrigin(const BuildIdentity& build)
{
    switch (build.origin)
    {
    case BuildOrigin::Release: return "Official release";
    case BuildOrigin::Test: return "Test build of pull request #" + std::to_string(build.pull_request);
    case BuildOrigin::Dev: break;
    }
    return build.run ? "Development build (CI run " + std::to_string(build.run) + ")" : "Local build";
}

std::string DescribeBuild(const BuildIdentity& build)
{
    std::string text = DescribeOrigin(build);
    if (!build.commit.empty()) text += ", commit " + Short(build.commit);
    if (build.built) text += ", built " + FormatUtcTime(build.built);
    else if (build.commit_time) text += " from " + FormatUtcTime(build.commit_time);
    return text;
}

UpdateChannel ResolveChannel(const std::string& setting, const BuildIdentity& build)
{
    if (setting == "release") return UpdateChannel::Release;
    if (setting == "test") return UpdateChannel::Test;
    return build.origin == BuildOrigin::Test ? UpdateChannel::Test : UpdateChannel::Release;
}

const PackageAsset* PublishedBuild::PackageFor(const std::string& package) const
{
    if (package.empty()) return nullptr;
    for (const auto& asset : assets)
    {
        std::string text;
        if (!PackageVersion(asset.name, text)) continue;
        const std::string stem = "mscharged-" + text + "-" + package;
        if (asset.name == stem + ".zip" || asset.name == stem + ".tar.gz") return &asset;
    }
    return nullptr;
}

const PackageAsset* PublishedBuild::Checksums() const
{
    for (const auto& asset : assets)
        if (asset.name == "SHA256SUMS") return &asset;
    return nullptr;
}

std::vector<PublishedBuild> ParseReleases(const platform::JsonValue& releases)
{
    std::vector<PublishedBuild> builds;
    for (const auto& item : releases.Items())
    {
        if (!item.IsObject() || item["draft"].Bool()) continue;
        PublishedBuild build;
        build.tag = item["tag_name"].Text();
        std::string tag_commit;
        build.test = ParseTestTag(build.tag, build.pull_request, tag_commit);
        // Test builds are prereleases; other prereleases are not offered.
        if (item["prerelease"].Bool() != build.test) continue;
        build.title = item["name"].Text();
        build.page = item["html_url"].Text();
        build.notes = item["body"].Text();
        build.published = ParseUtcTime(item["published_at"].Text());
        for (const auto& entry : item["assets"].Items())
        {
            PackageAsset asset;
            asset.name = entry["name"].Text();
            asset.url = entry["browser_download_url"].Text();
            asset.digest = entry["digest"].Text();
            asset.size = std::uint64_t(std::max<std::int64_t>(entry["size"].Integer().value_or(0), 0));
            if (asset.name.empty() || asset.url.rfind("https://", 0) != 0) continue;
            std::string version;
            if (!build.version.Valid() && PackageVersion(asset.name, version)) build.version = ParsePortVersion(version);
            build.assets.push_back(std::move(asset));
        }
        if (!build.version.Valid()) continue;
        // A test build's packages carry the commit its tag names.
        if (build.test && build.version.commit.empty()) build.version.commit = tag_commit;
        builds.push_back(std::move(build));
    }
    return builds;
}

PullRequestInfo ParsePullRequest(const platform::JsonValue& pull)
{
    PullRequestInfo info;
    info.number = int(pull["number"].Integer().value_or(0));
    info.title = pull["title"].Text();
    info.body = pull["body"].Text();
    info.page = pull["html_url"].Text();
    info.open = pull["state"].Text() == "open";
    info.merged = pull["merged"].Bool() || !pull["merged_at"].IsNull();
    return info;
}

const PublishedBuild* FindUpdate(const BuildIdentity& build, UpdateChannel channel,
                                 const std::vector<PublishedBuild>& published)
{
    const PortVersion self = ParsePortVersion(build.version);
    if (!self.Valid()) return nullptr;
    // A test build follows its own pull request while that has test builds.
    bool own_pull_request = false;
    if (channel == UpdateChannel::Test && build.origin == BuildOrigin::Test)
        for (const auto& candidate : published)
            own_pull_request = own_pull_request || (candidate.test && candidate.pull_request == build.pull_request
                && candidate.PackageFor(build.package));
    const PublishedBuild* best = nullptr;
    for (const auto& candidate : published)
    {
        if (!candidate.PackageFor(build.package)) continue;
        if (candidate.test && (channel == UpdateChannel::Release
            || (own_pull_request && candidate.pull_request != build.pull_request)))
            continue;
        if (!best || Newer(candidate, *best)) best = &candidate;
    }
    if (!best) return nullptr;
    // This very build (same commit).
    if (!best->version.commit.empty() && !self.commit.empty()
        && (best->version.commit.rfind(self.commit, 0) == 0 || self.commit.rfind(best->version.commit, 0) == 0))
        return nullptr;
    const int order = CompareVersions(best->version, self);
    if (order != 0) return order > 0 ? best : nullptr;
    if (build.origin == BuildOrigin::Dev) return nullptr;
    return build.Time() && best->published > build.Time() ? best : nullptr;
}

std::string ExpectedSha256(const PackageAsset& asset, std::string_view checksums)
{
    auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    };
    if (asset.digest.rfind("sha256:", 0) == 0 && asset.digest.size() == 7 + 64 && IsHex(asset.digest.substr(7)))
        return lower(asset.digest.substr(7));
    // sha256sum lines: "<hex>  <name>" (or " *<name>" for binary mode).
    while (!checksums.empty())
    {
        const auto end = checksums.find('\n');
        std::string_view line = checksums.substr(0, end);
        checksums = end == std::string_view::npos ? std::string_view() : checksums.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() < 66 || !IsHex(line.substr(0, 64)) || line[64] != ' ') continue;
        std::string_view name = line.substr(65);
        if (!name.empty() && (name[0] == ' ' || name[0] == '*')) name.remove_prefix(1);
        if (name == asset.name) return lower(std::string(line.substr(0, 64)));
    }
    return {};
}

std::int64_t ParseUtcTime(std::string_view text)
{
    // YYYY-MM-DDTHH:MM:SS followed by Z (GitHub's format)
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':'
        || text[16] != ':' || text[19] != 'Z' || !ParseNumber(text.substr(0, 4), year)
        || !ParseNumber(text.substr(5, 2), month) || !ParseNumber(text.substr(8, 2), day)
        || !ParseNumber(text.substr(11, 2), hour) || !ParseNumber(text.substr(14, 2), minute)
        || !ParseNumber(text.substr(17, 2), second) || month < 1 || month > 12 || day < 1 || day > 31
        || hour > 23 || minute > 59 || second > 60)
        return 0;
    return DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

std::string FormatUtcTime(std::int64_t seconds)
{
    std::int64_t days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) { rest += 86400; --days; }
    // Civil date from days since 1970-01-01 (H. Hinnant's algorithm).
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = int(doy - (153 * mp + 2) / 5 + 1);
    const int month = int(mp < 10 ? mp + 3 : mp - 9);
    const long long year = (long long)(yoe + era * 400 + (month <= 2));
    char text[48];
    std::snprintf(text, sizeof text, "%04lld-%02d-%02d %02d:%02d UTC", year, month, day, int(rest / 3600),
                  int(rest % 3600 / 60));
    return text;
}

std::string ReleasesApiUrl(const std::string& repository)
{
    return "https://api.github.com/repos/" + repository + "/releases?per_page=30";
}

std::string PullRequestApiUrl(const std::string& repository, int number)
{
    return "https://api.github.com/repos/" + repository + "/pulls/" + std::to_string(number);
}

std::string ReleasesPageUrl(const std::string& repository) { return "https://github.com/" + repository + "/releases"; }

std::string PullRequestPageUrl(const std::string& repository, int number)
{
    return "https://github.com/" + repository + "/pull/" + std::to_string(number);
}

std::string BuildNotesUrl(const BuildIdentity& build)
{
    if (build.origin == BuildOrigin::Test && build.pull_request)
        return PullRequestPageUrl(build.repository, build.pull_request);
    const PortVersion version = ParsePortVersion(build.version);
    if (build.origin == BuildOrigin::Release && version.Valid())
        return ReleasesPageUrl(build.repository) + "/tag/v" + VersionNumber(version);
    return ReleasesPageUrl(build.repository);
}
} // namespace mscharged::launcher
