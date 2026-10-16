// The launcher's update check and in-place installation (#50), without network:
// JSON, SHA-256, GitHub's release and pull request answers, the choice of an
// update, release notes, and unpacking and installing packages that
// tests/launcher_update_fixture.py writes the way tools/package_build.py does.
#include "launcher/release_notes.h"
#include "launcher/update_check.h"
#include "launcher/update_install.h"
#include "platform/http_client.h"
#include "platform/json.h"
#include "platform/path.h"
#include "platform/sha256.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace mscharged;
using namespace mscharged::launcher;
namespace fs = std::filesystem;

namespace
{
int failures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

bool Throws(const std::function<void()>& action)
{
    try { action(); }
    catch (const std::exception&) { return true; }
    return false;
}

std::string Read(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    std::stringstream text;
    text << stream.rdbuf();
    return text.str();
}

// Every file below `root` with its contents, by relative path.
std::map<std::string, std::string> Tree(const fs::path& root)
{
    std::map<std::string, std::string> files;
    if (!fs::exists(root)) return files;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) files[fs::relative(entry.path(), root).generic_string()] = Read(entry.path());
    return files;
}

void TestJson()
{
    const auto value = platform::ParseJson(
        R"({"name": "Strikers \"Charged\"", "list": [1, -2, 3.5e2, true, null], "nested": {"a": {}},
            "text": "caf\u00e9 \ud83d\udc4d \/", "big": 38790034498})");
    Check(value["name"].Text() == "Strikers \"Charged\"", "JSON string escapes");
    Check(value["list"].size() == 5 && value["list"].Items()[1].Integer() == -2, "JSON array and integers");
    Check(!value["list"].Items()[2].Integer() && value["list"].Items()[2].Text() == "3.5e2", "JSON number text");
    Check(value["list"].Items()[3].Bool() && value["list"].Items()[4].IsNull(), "JSON literals");
    Check(value["nested"]["a"].IsObject() && value["missing"]["deeper"].IsNull(), "JSON missing members");
    Check(value["text"].Text() == "caf\xC3\xA9 \xF0\x9F\x91\x8D /", "JSON unicode escapes and surrogate pairs");
    Check(value["big"].Integer() == 38790034498LL, "JSON 64-bit integers");
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "01", "\"\x01\"", "[1] x", "{\"a\":tru}", "\"\\x\""})
        Check(Throws([&] { platform::ParseJson(bad); }), std::string("JSON rejects ") + bad);
    Check(Throws([] { platform::ParseJson(std::string(100, '[') + std::string(100, ']')); }), "JSON depth limit");
}

void TestSha256()
{
    Check(platform::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty");
    Check(platform::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");
    Check(platform::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
              == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 two blocks");
    platform::Sha256 hash;
    const std::string chunk(1000, 'a');
    for (int n = 0; n < 1000; ++n) hash.Update(chunk.data(), chunk.size());
    Check(hash.FinishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 a million a");
}

void TestVersions()
{
    const auto version = ParsePortVersion("1.0.10+g7861e603a8ec");
    Check(version.Valid() && version.major == 1 && version.patch == 10 && version.commit == "7861e603a8ec",
          "Version with commit");
    Check(ParsePortVersion("v1.2.3").minor == 2 && ParsePortVersion("1.0.9+g6144eb682311.dirty").commit == "6144eb682311",
          "Tag and dirty versions");
    for (const char* bad : {"", "1", "1.0", "1.0.0.0", "1.x.0", "1.0.0+main", "1.0.0+g", "-1.0.0"})
        Check(!ParsePortVersion(bad).Valid(), std::string("Version rejects ") + bad);
    Check(CompareVersions(ParsePortVersion("1.0.10"), ParsePortVersion("1.0.9")) > 0, "1.0.10 > 1.0.9");
    Check(CompareVersions(ParsePortVersion("1.1.0"), ParsePortVersion("1.0.99")) > 0, "1.1.0 > 1.0.99");
    Check(CompareVersions(ParsePortVersion("1.0.9+gaaaa"), ParsePortVersion("1.0.9+gbbbb")) == 0, "Commits do not order");
    Check(VersionNumber(version) == "1.0.10", "Version number text");
    Check(ParseUtcTime("2026-10-15T17:22:00Z") == 1792084920 && ParseUtcTime("1970-01-01T00:00:00Z") == 0
              && ParseUtcTime("2026-10-15 17:22:00") == 0,
          "UTC times");
    Check(FormatUtcTime(1792084920) == "2026-10-15 17:22 UTC" && FormatUtcTime(951782400) == "2000-02-29 00:00 UTC",
          "UTC time text");
}

std::string Release(const std::string& tag, bool prerelease, const std::string& published, const std::string& version,
                    const std::string& body = "Notes", bool draft = false, bool digest = true)
{
    std::string assets;
    for (const char* package : {"linux-x86_64", "steamdeck-x86_64", "macos-arm64", "windows-x86_64"})
    {
        const std::string extension = std::string(package).rfind("windows", 0) == 0 ? ".zip" : ".tar.gz";
        const std::string name = "mscharged-" + version + "-" + package + extension;
        assets += std::string(assets.empty() ? "" : ",") + R"({"name":")" + name
            + R"(","browser_download_url":"https://github.com/o/r/releases/download/)" + tag + "/" + name
            + R"(","size":1234,"digest":)" + (digest ? "\"sha256:" + std::string(64, 'a') + "\"" : "null") + "}";
    }
    assets += R"(,{"name":"SHA256SUMS","browser_download_url":"https://github.com/o/r/releases/download/)" + tag
        + R"(/SHA256SUMS","size":10,"digest":null})";
    return R"({"tag_name":")" + tag + R"(","name":"Title )" + tag + R"(","draft":)" + (draft ? "true" : "false")
        + R"(,"prerelease":)" + (prerelease ? "true" : "false") + R"(,"html_url":"https://github.com/o/r/releases/tag/)"
        + tag + R"(","published_at":")" + published + R"(","body":")" + body + R"(","assets":[)" + assets + "]}";
}

BuildIdentity Build(BuildOrigin origin, const std::string& version, std::int64_t built, int pull_request = 0,
                    const std::string& package = "linux-x86_64")
{
    BuildIdentity build;
    build.origin = origin;
    build.version = version;
    build.built = built;
    build.pull_request = pull_request;
    build.repository = "o/r";
    build.package = package;
    return build;
}

void TestReleases()
{
    const std::string list = "[" + Release("test-pr48-cccccccccccc", true, "2026-10-16T10:00:00Z", "1.0.10+gcccccccccccc")
        + "," + Release("test-pr50-dddddddddddd", true, "2026-10-16T12:00:00Z", "1.0.11+gdddddddddddd") + ","
        + Release("v1.0.10", false, "2026-10-15T18:00:00Z", "1.0.10+gbbbbbbbbbbbb", "## Fixes\\n- **One**", true) + ","
        + Release("v1.0.9", false, "2026-10-15T17:22:00Z", "1.0.9+gaaaaaaaaaaaa") + ","
        + Release("nightly", true, "2026-10-17T00:00:00Z", "1.0.12+geeeeeeeeeeee") + ","
        + Release("v1.0.8", false, "2026-10-13T20:00:38Z", "1.0.8+g999999999999", "", false, false) + "]";
    const auto builds = ParseReleases(platform::ParseJson(list));
    Check(builds.size() == 4, "Drafts and other prereleases are left out");
    const PublishedBuild* pr48 = nullptr;
    const PublishedBuild* release = nullptr;
    for (const auto& build : builds)
    {
        if (build.tag == "test-pr48-cccccccccccc") pr48 = &build;
        if (build.tag == "v1.0.9") release = &build;
    }
    Check(pr48 && pr48->test && pr48->pull_request == 48 && pr48->version.commit == "cccccccccccc", "Test build tag");
    Check(release && !release->test && release->version.patch == 9 && release->published == 1792084920,
          "Release fields");
    Check(release && release->PackageFor("steamdeck-x86_64")
              && release->PackageFor("steamdeck-x86_64")->name == "mscharged-1.0.9+gaaaaaaaaaaaa-steamdeck-x86_64.tar.gz"
              && release->PackageFor("windows-x86_64")->name.find(".zip") != std::string::npos
              && !release->PackageFor("macos-x86_64") && release->Checksums(),
          "Package per platform");

    // Release builds: newer versions only; the very same build is current.
    Check(!FindUpdate(Build(BuildOrigin::Release, "1.0.9+gaaaaaaaaaaaa", 1792084000), UpdateChannel::Release, builds),
          "A release is up to date with itself");
    const auto* update = FindUpdate(Build(BuildOrigin::Release, "1.0.8+g999999999999", 1791000000),
                                    UpdateChannel::Release, builds);
    Check(update && update->tag == "v1.0.9", "A release finds the newest release");
    update = FindUpdate(Build(BuildOrigin::Release, "1.0.9+gaaaaaaaaaaaa", 1792084000), UpdateChannel::Test, builds);
    Check(update && update->tag == "test-pr50-dddddddddddd", "The test channel offers the newest test build");
    Check(!FindUpdate(Build(BuildOrigin::Release, "1.0.9+gaaaaaaaaaaaa", 1792084000, 0, "macos-x86_64"),
                      UpdateChannel::Release, builds),
          "No package for this platform, no update");

    // A test build follows its pull request while that has test builds.
    update = FindUpdate(Build(BuildOrigin::Test, "1.0.10+gffffffffffff", 1792140000, 48), UpdateChannel::Test, builds);
    Check(update && update->tag == "test-pr48-cccccccccccc", "A test build follows its own pull request");
    Check(!FindUpdate(Build(BuildOrigin::Test, "1.0.10+gcccccccccccc", 1792140000, 48), UpdateChannel::Test, builds),
          "The newest build of the pull request is current");
    update = FindUpdate(Build(BuildOrigin::Test, "1.0.10+gffffffffffff", 1792140000, 47), UpdateChannel::Test, builds);
    Check(update && update->tag == "test-pr50-dddddddddddd", "A closed pull request's build takes the newest test build");

    // What an offer means: the dialog's update or switch.
    const BuildIdentity test48 = Build(BuildOrigin::Test, "1.0.10+gffffffffffff", 1792140000, 48);
    const BuildIdentity release9 = Build(BuildOrigin::Release, "1.0.9+gaaaaaaaaaaaa", 1792084000);
    update = FindUpdate(test48, UpdateChannel::Test, builds);
    Check(update && ClassifyOffer(test48, *update) == OfferKind::Update, "A newer build of the own pull request updates");
    update = FindUpdate(Build(BuildOrigin::Test, "1.0.10+gffffffffffff", 1792140000, 47), UpdateChannel::Test, builds);
    Check(update && ClassifyOffer(Build(BuildOrigin::Test, "1.0.10+gffffffffffff", 1792140000, 47), *update)
              == OfferKind::TestBuild,
          "Another pull request's test build is a switch");
    update = FindUpdate(release9, UpdateChannel::Test, builds);
    Check(update && ClassifyOffer(release9, *update) == OfferKind::TestBuild, "A release switches to a test build");

    // Back from a test build: the newest release, also an older one.
    update = FindUpdate(test48, UpdateChannel::Release, builds);
    Check(update && update->tag == "v1.0.9" && ClassifyOffer(test48, *update) == OfferKind::BackToRelease
              && !IsNewer(test48, *update),
          "On releases a test build is offered the way back to the newest release");
    Check(!UpdatesItself(update->version) && UpdatesItself(ParsePortVersion("1.0.10"))
              && UpdatesItself(ParsePortVersion("1.1.0")) && !UpdatesItself(ParsePortVersion("bad")),
          "In-app updates since 1.0.10");
    Check(!FindUpdate(Build(BuildOrigin::Dev, "1.0.10+gffffffffffff", 1792140000), UpdateChannel::Release, builds),
          "Local builds are not offered older releases");
    Check(!FindUpdate(Build(BuildOrigin::Release, "1.0.10+gffffffffffff", 1792140000), UpdateChannel::Release, builds),
          "Releases are not offered older releases");
    const auto same_version = ParseReleases(platform::ParseJson(
        "[" + Release("v1.0.10", false, "2026-10-16T08:00:00Z", "1.0.10+gbbbbbbbbbbbb") + "]"));
    update = FindUpdate(test48, UpdateChannel::Release, same_version);
    Check(update && ClassifyOffer(test48, *update) == OfferKind::BackToRelease,
          "A test build made after the release of its version goes back to that release");

    // Merged and released: the release of the same version is the update.
    const auto released = ParseReleases(platform::ParseJson(
        "[" + Release("v1.0.10", false, "2026-10-16T18:00:00Z", "1.0.10+gbbbbbbbbbbbb") + ","
        + Release("v1.0.9", false, "2026-10-15T17:22:00Z", "1.0.9+gaaaaaaaaaaaa") + "]"));
    update = FindUpdate(Build(BuildOrigin::Test, "1.0.10+gcccccccccccc", 1792140000, 48), UpdateChannel::Test, released);
    Check(update && update->tag == "v1.0.10", "A test build moves on to its release");
    update = FindUpdate(Build(BuildOrigin::Test, "1.0.10+gcccccccccccc", 1792140000, 48), UpdateChannel::Release, released);
    Check(update && update->tag == "v1.0.10"
              && ClassifyOffer(Build(BuildOrigin::Test, "1.0.10+gcccccccccccc", 1792140000, 48), *update)
                  == OfferKind::Update,
          "The later release of a tested version is an update on both channels");
    Check(!FindUpdate(Build(BuildOrigin::Dev, "1.0.10+gcccccccccccc", 1792140000), UpdateChannel::Release, released),
          "Local builds are only offered higher versions");
    Check(FindUpdate(Build(BuildOrigin::Dev, "1.0.9+gcccccccccccc", 0), UpdateChannel::Release, released),
          "Local builds are offered higher versions");
    Check(!FindUpdate(Build(BuildOrigin::Release, "not a version", 0), UpdateChannel::Release, released),
          "An unknown own version offers nothing");

    Check(ResolveChannel("auto", Build(BuildOrigin::Test, "1.0.0", 0, 1)) == UpdateChannel::Test
              && ResolveChannel("auto", Build(BuildOrigin::Dev, "1.0.0", 0)) == UpdateChannel::Release
              && ResolveChannel("test", Build(BuildOrigin::Release, "1.0.0", 0)) == UpdateChannel::Test
              && ResolveChannel("release", Build(BuildOrigin::Test, "1.0.0", 0, 1)) == UpdateChannel::Release,
          "Channel setting");

    PackageAsset asset;
    asset.name = "mscharged-1.0.9+gaaaaaaaaaaaa-linux-x86_64.tar.gz";
    asset.digest = "sha256:" + std::string(64, 'A');
    Check(ExpectedSha256(asset, {}) == std::string(64, 'a'), "GitHub's digest");
    asset.digest.clear();
    const std::string sums = std::string(64, 'b') + "  mscharged-1.0.9+gaaaaaaaaaaaa-linux-x86_64.tar.gz\n"
        + std::string(64, 'c') + " *other.zip\r\n";
    Check(ExpectedSha256(asset, sums) == std::string(64, 'b'), "SHA256SUMS line");
    asset.name = "other.zip";
    Check(ExpectedSha256(asset, sums) == std::string(64, 'c'), "SHA256SUMS binary-mode line");
    asset.name = "missing.zip";
    Check(ExpectedSha256(asset, sums).empty(), "No checksum");

    const auto pull = ParsePullRequest(platform::ParseJson(
        R"({"number":48,"title":"Release 1.0.10","body":"Version 1.0.10 ...","html_url":"https://github.com/o/r/pull/48",
            "state":"closed","merged_at":"2026-10-16T17:00:00Z"})"));
    Check(pull.number == 48 && !pull.open && pull.merged && pull.body == "Version 1.0.10 ...", "Pull request fields");
    auto build = Build(BuildOrigin::Test, "1.0.10+gcccccccccccc", 1792140000, 48);
    Check(BuildNotesUrl(build) == "https://github.com/o/r/pull/48", "Test build notes");
    build.origin = BuildOrigin::Release;
    Check(BuildNotesUrl(build) == "https://github.com/o/r/releases/tag/v1.0.10", "Release notes");
    build.origin = BuildOrigin::Dev;
    Check(BuildNotesUrl(build) == "https://github.com/o/r/releases", "Local build notes");
    build.commit = "cccccccccccccccccccccccccccccccccccccccc";
    Check(DescribeBuild(build) == "Local build, commit cccccccccccc, built 2026-10-16 08:40 UTC", "Build description");

    // The notice after an update or a switch.
    Check(DescribeReplacement("1.0.9+gaaaaaaaaaaaa", Build(BuildOrigin::Release, "1.0.10+gbbbbbbbbbbbb", 0))
              == "Updated from 1.0.9 to release 1.0.10.",
          "Update notice");
    Check(DescribeReplacement("1.0.10+gcccccccccccc", Build(BuildOrigin::Test, "1.0.10+gdddddddddddd", 0, 48))
              == "Updated from 1.0.10 (cccccccc) to test build 1.0.10 (dddddddd).",
          "Same-version notice names the commits");
    Check(DescribeReplacement("1.0.11+gcccccccccccc", Build(BuildOrigin::Release, "1.0.10+gbbbbbbbbbbbb", 0))
              == "Switched from 1.0.11 to release 1.0.10.",
          "Back-to-release notice");
    Check(DescribeReplacement("garbage", Build(BuildOrigin::Release, "1.0.10", 0)).empty(), "Unknown replaced version");
}

void TestNotes()
{
    const auto blocks = ParseReleaseNotes(
        "Version 1.0.10 prepares\n11 more pipelines.\r\n\r\n## Graphics preparation\n\n"
        "- **11 more pipelines (#46):** a tester's `mscharged.log` and [the catalogue](https://x/y) <!-- hidden -->\n"
        "  continue here\n  - nested\n1. numbered\n\n---\n```\ncode line\n```\n![image](https://x/i.png)\n"
        "<details><summary>More</summary>\n\nHidden *details* \\*star\\*\n</details>\n");
    Check(blocks.size() >= 8, "Note blocks");
    Check(blocks[0].kind == NoteBlock::Kind::Paragraph && blocks[0].spans.size() == 1
              && blocks[0].spans[0].text == "Version 1.0.10 prepares 11 more pipelines.",
          "Paragraph lines join");
    Check(blocks[1].kind == NoteBlock::Kind::Heading && blocks[1].level == 2 && blocks[1].spans[0].text == "Graphics preparation",
          "Heading");
    const auto& item = blocks[2];
    Check(item.kind == NoteBlock::Kind::Item && item.level == 0 && item.marker.empty() && item.spans.size() >= 5
              && item.spans[0].style == NoteSpan::Style::Strong && item.spans[0].text == "11 more pipelines (#46):"
              && item.spans[2].style == NoteSpan::Style::Code && item.spans[2].text == "mscharged.log"
              && item.spans[4].style == NoteSpan::Style::Link && item.spans[4].text == "the catalogue",
          "List item spans");
    Check(item.spans.back().text.find("continue here") != std::string::npos
              && item.spans.back().text.find("hidden") == std::string::npos,
          "Item continuation without comments");
    Check(blocks[3].kind == NoteBlock::Kind::Item && blocks[3].level == 1, "Nested item");
    Check(blocks[4].kind == NoteBlock::Kind::Item && blocks[4].marker == "1.", "Numbered item");
    Check(blocks[5].kind == NoteBlock::Kind::Rule, "Rule");
    Check(blocks[6].kind == NoteBlock::Kind::Code && blocks[6].spans[0].text == "code line", "Code line");
    bool image = false, details = false;
    for (const auto& block : blocks)
        for (const auto& span : block.spans)
        {
            image = image || span.text.find("image") != std::string::npos || span.text.find("i.png") != std::string::npos;
            details = details || span.text == "More" || span.text.find("Hidden details *star*") != std::string::npos;
        }
    Check(!image && details, "Images dropped, HTML tags stripped, escapes kept");
    Check(ParseReleaseNotes("").empty() && ParseReleaseNotes("\n\n  \n").empty(), "Empty notes");
}

fs::path Fresh(const fs::path& root, const std::string& name)
{
    const fs::path path = root / name;
    fs::remove_all(path);
    return path;
}

void TestUnpack(const fs::path& fixtures, const fs::path& work)
{
    const std::string linux_name = "mscharged-1.0.10+gbbbbbbbbbbbb-linux-x86_64";
    const fs::path linux_root = UnpackPackage(fixtures / (linux_name + ".tar.gz"), Fresh(work, "unpack-linux"));
    Check(linux_root.filename() == linux_name, "tar.gz packages unpack into their folder");
    const auto manifest = ReadPackageManifest(linux_root / kPackageManifest);
    Check(manifest.Package() == "linux-x86_64" && manifest.version == "1.0.10+gbbbbbbbbbbbb" && manifest.files.size() == 5,
          "Unpacked manifest");
    Check(Read(linux_root / "docs/NEW.md") == "only in the new package\n", "Unpacked file");
    Check(fs::exists(linux_root / ("LICENSES/dependencies/" + std::string(60, 'a') + "/" + std::string(60, 'b') + "/LICENSE")),
          "Long pax path");
#ifndef _WIN32
    Check((fs::status(linux_root / "mscharged").permissions() & fs::perms::owner_exec) != fs::perms::none
              && (fs::status(linux_root / "docs/NEW.md").permissions() & fs::perms::owner_exec) == fs::perms::none,
          "Manifest modes");
#endif
    const fs::path windows_root =
        UnpackPackage(fixtures / "mscharged-1.0.10+gbbbbbbbbbbbb-windows-x86_64.zip", Fresh(work, "unpack-windows"));
    Check(windows_root == work / "unpack-windows" && Read(windows_root / "mscharged_original_frontend_module.so").size() == 11000,
          "zip packages unpack at their root");

    for (const char* bad : {"bad-traversal.tar.gz", "bad-symlink.tar.gz", "bad-hash.tar.gz", "bad-absolute.tar.gz",
                            "bad-unlisted.zip", "bad-truncated.tar.gz"})
        Check(Throws([&] { UnpackPackage(fixtures / bad, Fresh(work, std::string("unpack-") + bad)); }),
              std::string("Refuses ") + bad);
    Check(!fs::exists(work / "evil") && !fs::exists(work.parent_path() / "evil") && !fs::exists("/tmp/mscharged-absolute"),
          "Nothing escapes the unpack folder");
    Check(Throws([&] { UnpackPackage(fixtures / (linux_name + ".tar.gz"), work / "unpack-linux"); }),
          "The unpack folder must be new");
    bool cancelled = Throws([&] {
        UnpackPackage(fixtures / (linux_name + ".tar.gz"), Fresh(work, "unpack-cancelled"), [] { return true; });
    });
    Check(cancelled, "Unpacking can be cancelled");
}

fs::path InstalledCopy(const fs::path& fixtures, const fs::path& work, const std::string& name)
{
    const fs::path base = Fresh(work, name);
    fs::copy(fixtures / "installed", base, fs::copy_options::recursive);
    return base;
}

void TestInstall(const fs::path& fixtures, const fs::path& work)
{
    const std::string archive = "mscharged-1.0.10+gbbbbbbbbbbbb-linux-x86_64.tar.gz";
    const auto installed = Tree(fixtures / "installed");

    // A complete update: package files replaced, user files untouched.
    {
        const fs::path base = InstalledCopy(fixtures, work, "install");
        Check(InstallBlocker(base).empty() || InstallBlocker(base).rfind("Downloads are not available", 0) == 0,
              "A package folder can be updated");
        const fs::path root = UnpackPackage(fixtures / archive, UpdateFolder(base) / "staged-1");
        InstallStagedPackage(base, root, UpdateFolder(base) / "previous-1");
        Check(Read(base / "mscharged") == "new launcher\n" && Read(base / "docs/NEW.md") == "only in the new package\n"
                  && !fs::exists(base / "docs/OLD.md"),
              "New files in place, removed files gone");
        Check(ReadPackageManifest(base / kPackageManifest).version == "1.0.10+gbbbbbbbbbbbb", "New manifest in place");
        Check(Read(base / "mscharged.ini") == installed.at("mscharged.ini")
                  && Read(base / "original-main-credits-data/nand/save.bin") == "save data"
                  && Read(base / "logs/mscharged.log") == "previous log\n",
              "Settings, saves and logs stay");
#ifndef _WIN32
        Check((fs::status(base / "mscharged").permissions() & fs::perms::owner_exec) != fs::perms::none,
              "The launcher stays executable");
#endif
        Check(!fs::exists(UpdateFolder(base) / "replacing"), "No replacement marker after success");
        Check(FinishUpdates(base) == "1.0.9+gaaaaaaaaaaaa", "The next start reports the replaced version");
        Check(!fs::exists(UpdateFolder(base)), "The next start removes the update folder");
        Check(FinishUpdates(base).empty(), "Nothing to report afterwards");
    }

    // A failure half way moves everything back.
    {
        const fs::path base = InstalledCopy(fixtures, work, "rollback");
        const fs::path root = UnpackPackage(fixtures / archive, UpdateFolder(base) / "staged-1");
        const auto staged = Tree(root);
        int moves = 0;
        Check(Throws([&] {
                  ReplacePackage(base, root, UpdateFolder(base) / "previous-1", [&](const std::string&) {
                      if (++moves == 4) throw std::runtime_error("disk full");
                  });
              }),
              "A failing move fails the replacement");
        auto after = Tree(base);
        for (auto it = after.begin(); it != after.end();)
            it = it->first.rfind(".update/", 0) == 0 ? after.erase(it) : std::next(it);
        Check(after == installed, "The installed files are back unchanged");
        Check(Tree(root) == staged, "The unpacked package is back in its folder");
        Check(Tree(UpdateFolder(base) / "previous-1").empty(), "No previous files are left behind");
    }

    // An interrupted replacement (the launcher was killed) is completed at the next start.
    {
        const fs::path base = InstalledCopy(fixtures, work, "interrupted");
        const fs::path root = UnpackPackage(fixtures / archive, UpdateFolder(base) / "staged-1");
        const fs::path previous = UpdateFolder(base) / "previous-1";
        fs::create_directories(previous / "docs");
        fs::rename(base / "docs/OLD.md", previous / "docs/OLD.md");
        fs::rename(base / "mscharged", previous / "mscharged"); // moved aside, the new one not yet in
        std::ofstream(UpdateFolder(base) / "replacing") << "staged-1/" << root.filename().generic_string() << "\nprevious-1\n";
        Check(FinishUpdates(base) == "1.0.9+gaaaaaaaaaaaa", "The interrupted update is completed");
        Check(Read(base / "mscharged") == "new launcher\n" && Read(base / "docs/NEW.md") == "only in the new package\n"
                  && !fs::exists(base / "docs/OLD.md") && Read(base / "mscharged.ini") == installed.at("mscharged.ini")
                  && ReadPackageManifest(base / kPackageManifest).version == "1.0.10+gbbbbbbbbbbbb",
              "Completed files");
        Check(!fs::exists(UpdateFolder(base)), "Completed update cleaned up");
    }

    // The package must be for this folder's platform.
    {
        const fs::path base = InstalledCopy(fixtures, work, "mismatch");
        const fs::path root = UnpackPackage(fixtures / "mscharged-1.0.10+gbbbbbbbbbbbb-steamdeck-x86_64.tar.gz",
                                            UpdateFolder(base) / "staged-1");
        Check(Throws([&] { ReplacePackage(base, root, UpdateFolder(base) / "previous-1"); }),
              "A package for another platform is refused");
        auto after = Tree(base);
        for (auto it = after.begin(); it != after.end();)
            it = it->first.rfind(".update/", 0) == 0 ? after.erase(it) : std::next(it);
        Check(after == installed, "Nothing changed");
    }

    // Folders that were not installed from a package are not updated in place.
    const fs::path plain = Fresh(work, "plain");
    fs::create_directories(plain);
    Check(!InstallBlocker(plain).empty(), "No manifest, no in-place update");
    Check(FinishUpdates(plain).empty(), "Nothing to finish");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: launcher_update_tests FIXTURE_DIR (see tests/launcher_update_fixture.py)\n";
        return 2;
    }
    const fs::path fixtures = fs::absolute(argv[1]);
    const fs::path work = fixtures / "work";
    fs::create_directories(work);
    try
    {
        TestJson();
        TestSha256();
        TestVersions();
        TestReleases();
        TestNotes();
        TestUnpack(fixtures, work);
        TestInstall(fixtures, work);
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED with exception: " << error.what() << '\n';
        return 1;
    }
    if (failures)
    {
        std::cerr << failures << " launcher update check(s) failed\n";
        return 1;
    }
    std::cout << "Launcher update checks passed.\n";
    return 0;
}
