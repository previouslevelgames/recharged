// The launcher's image library (#65): scan, cache, order and banners. Without
// arguments it runs the checks below on files with made-up contents. With
// "probe IMAGE", "banner IMAGE ID OUT" or "scan FOLDER" it runs the real disc
// reader on synthetic discs for tests/test_launcher_image_library.py.
#include "launcher/image_library.h"
#include "launcher/release_banner.h"
#include "platform/path.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace mscharged;
using namespace mscharged::launcher;
namespace fs = std::filesystem;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void MakeFile(const fs::path& path, std::uintmax_t size)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('x');
    fs::resize_file(path, size);
}

constexpr std::uintmax_t kImage = 40ull << 20;

void Put32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value)
{
    for (int n = 0; n < 4; ++n) data[offset + std::size_t(n)] = std::uint8_t(value >> (24 - 8 * n));
}

// A TPL with one 192 x 64 RGB5A3 image whose texel n (in file order) is `texel(n)`.
std::vector<std::uint8_t> MakeTpl(std::uint16_t (*texel)(std::size_t), std::uint32_t format = 5, int width = 192)
{
    std::vector<std::uint8_t> data(0x40 + std::size_t(192 * 64 * 2));
    Put32(data, 0, 0x0020AF30);
    Put32(data, 4, 1);
    Put32(data, 8, 0x0C);
    Put32(data, 0x0C, 0x14);
    data[0x14] = 0;
    data[0x15] = 64;
    data[0x16] = std::uint8_t(width >> 8);
    data[0x17] = std::uint8_t(width);
    Put32(data, 0x18, format);
    Put32(data, 0x1C, 0x40);
    for (std::size_t n = 0; n < std::size_t(192 * 64); ++n)
    {
        const auto value = texel(n);
        data[0x40 + n * 2] = std::uint8_t(value >> 8);
        data[0x40 + n * 2 + 1] = std::uint8_t(value);
    }
    return data;
}

DiscProbe Charged(const char* id, std::uint8_t revision)
{
    DiscProbe probe;
    probe.kind = DiscProbe::Kind::Charged;
    probe.info = {"RVZ", id, "Mario Strikers Charged", revision, 2354};
    return probe;
}

std::string Names(const std::vector<const LibraryImage*>& images)
{
    std::string text;
    for (const auto* image : images) text += (text.empty() ? "" : ", ") + PathUtf8(image->path.filename());
    return text;
}

void CheckBanners()
{
    Require(ReleaseBannerFile("R4QE01") == "Art/fe/us_banner.tpl", "USA banner file");
    Require(ReleaseBannerFile("R4QK01") == "Art/fe/us_banner.tpl", "Korea takes the USA branch");
    Require(ReleaseBannerFile("R4QP01") == "Art/fe/eu_banner.tpl", "European banner file");
    Require(ReleaseBannerFile("R4QJ01") == "Art/fe/jp_banner.tpl", "Japanese banner file");
    Require(ReleaseBannerFile("R4QX01").empty() && ReleaseBannerFile("RMCE01").empty(), "No banner for others");

    // Texel 0 of the first tile: opaque RGB555 (31, 0, 16); texel 1: 3-bit
    // alpha 5 with RGB444 (15, 8, 1); everything else black.
    const auto tpl = MakeTpl([](std::size_t n) -> std::uint16_t {
        if (n == 0) return 0x8000 | 31 << 10 | 16;
        if (n == 1) return 5 << 12 | 15 << 8 | 8 << 4 | 1;
        if (n == 16) return 0xFFFF; // first texel of the second tile: x = 4, y = 0
        return 0x8000;
    });
    const auto rgba = DecodeBannerTpl(tpl.data(), tpl.size());
    Require(rgba.size() == std::size_t(kBannerWidth * kBannerHeight * 4), "Banner size");
    const auto pixel = [&](int x, int y) {
        const auto* p = &rgba[(std::size_t(y) * kBannerWidth + std::size_t(x)) * 4];
        return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
    };
    Require(pixel(0, 0) == 0xFF0084FF, "RGB555 expands 5 bits to 8");
    Require(pixel(1, 0) == 0xFF8811B6, "3-bit alpha and RGB444 expand to 8 bits");
    Require(pixel(4, 0) == 0xFFFFFFFF, "Tiles run left to right");
    Require(pixel(0, 1) == 0x000000FF, "The next row of the tile");
    // Damaged or different files.
    auto bad = tpl;
    bad[1] = 0;
    const auto rejects = [](const std::vector<std::uint8_t>& data) {
        try { DecodeBannerTpl(data.data(), data.size()); }
        catch (const std::runtime_error&) { return true; }
        return false;
    };
    Require(rejects(bad), "Wrong magic");
    Require(rejects(MakeTpl([](std::size_t) -> std::uint16_t { return 0; }, 14)), "Another texture format");
    Require(rejects(MakeTpl([](std::size_t) -> std::uint16_t { return 0; }, 5, 128)), "Another size");
    Require(rejects(std::vector<std::uint8_t>(tpl.begin(), tpl.end() - 1)), "Truncated pixels");
    Require(rejects(std::vector<std::uint8_t>(tpl.begin(), tpl.begin() + 10)), "Truncated header");
    Require(rejects({}), "Empty file");
    std::puts("Banner checks passed.");
}

void CheckCache()
{
    ImageCache cache;
    auto eu = Charged("R4QP01", 2);
    cache.Put("/games/eu.rvz", 1500, 42, eu);
    DiscProbe other;
    other.kind = DiscProbe::Kind::OtherGame;
    other.info = {"", "RMCE01", "Mario \"Kart\"\tWii", 0, 0};
    cache.Put("/games/kart.iso", 4000, 7, other);
    DiscProbe broken;
    broken.error = "Cannot open disc: unexpected end of file\nat byte 12";
    cache.Put("/games/broken.rvz", 50, -3, broken);
    DiscProbe unknown;
    unknown.kind = DiscProbe::Kind::NotDisc;
    unknown.error = "Unknown disc format";
    cache.Put(PathFromUtf8("/games/linux \xc3\xa9.iso"), 60, 1, unknown);

    const auto copy = ImageCache::Parse(cache.Serialize());
    Require(copy.size() == 4, "Every entry is kept");
    const auto* found = copy.Find("/games/eu.rvz", 1500, 42);
    Require(found && found->kind == DiscProbe::Kind::Charged && found->info.game_id == "R4QP01"
                && found->info.revision == 2 && found->info.format == "RVZ" && found->info.file_count == 2354,
            "A Charged entry round-trips");
    Require(!copy.Find("/games/eu.rvz", 1501, 42) && !copy.Find("/games/eu.rvz", 1500, 43),
            "A changed size or time misses");
    Require(copy.Last("/games/eu.rvz") && copy.Last("/games/eu.rvz")->size == 1500, "Last ignores size and time");
    found = copy.Find("/games/kart.iso", 4000, 7);
    Require(found && found->kind == DiscProbe::Kind::OtherGame && found->info.title == other.info.title,
            "Other games round-trip, quotes and tabs too");
    found = copy.Find("/games/broken.rvz", 50, -3);
    Require(found && found->kind == DiscProbe::Kind::Unreadable && found->error == broken.error,
            "Unreadable entries keep their error");
    found = copy.Find(PathFromUtf8("/games/linux \xc3\xa9.iso"), 60, 1);
    Require(found && found->kind == DiscProbe::Kind::NotDisc, "Non-ASCII paths round-trip");
    Require(ImageCache::Parse("not json").size() == 0 && ImageCache::Parse("{\"version\": 2}").size() == 0,
            "Unreadable cache files are empty caches");
    Require(ImageCache::Parse(R"({"version":1,"images":[{"path":"/a","size":1,"modified":1,"kind":"charged",)"
                              R"("id":"R4QE","revision":1,"format":"ISO","files":3}]})")
                    .size() == 0,
            "Entries with impossible values are dropped");
    std::puts("Cache checks passed.");
}

void CheckScan(const fs::path& root)
{
    // Files named after what the made-up reader reports for them.
    const std::map<std::string, DiscProbe> contents{
        {"us.rvz", Charged("R4QE01", 1)},  {"us copy.iso", Charged("R4QE01", 1)},
        {"eu2.rvz", Charged("R4QP01", 2)}, {"eu1.iso", Charged("R4QP01", 1)},
        {"jp.rvz", Charged("R4QJ01", 0)},  {"deep.rvz", Charged("R4QK01", 0)},
    };
    std::map<std::string, int> opened;
    const auto probe = [&](const fs::path& path) {
        ++opened[PathUtf8(path.filename())];
        const auto it = contents.find(PathUtf8(path.filename()));
        if (it != contents.end()) return it->second;
        DiscProbe result;
        if (path.filename() == "metroid.iso")
        {
            result.kind = DiscProbe::Kind::OtherGame;
            result.info.game_id = "GM8E01";
        }
        else if (path.filename() == "linux.iso") result.kind = DiscProbe::Kind::NotDisc;
        else result.error = "Cannot open disc: unexpected end of file";
        return result;
    };
    std::map<std::string, int> banner_reads;
    const auto banner = [&](const fs::path&, const std::string& id) {
        ++banner_reads[id];
        if (id == "R4QJ01") throw std::runtime_error("No banner");
        return std::vector<std::uint8_t>(4, std::uint8_t(id[3]));
    };
    const auto games = root / "Games", sd = root / "sd";
    MakeFile(games / "us.rvz", kImage);
    MakeFile(games / "us copy.iso", kImage + 1);
    MakeFile(games / "Europe" / "eu2.rvz", kImage + 2);
    MakeFile(games / "Europe" / "eu1.iso", kImage + 3);
    MakeFile(games / "metroid.iso", kImage);
    MakeFile(games / "linux.iso", kImage);
    MakeFile(games / "broken.rvz", kImage);
    MakeFile(games / "a" / "b" / "c" / "d" / "deep.rvz", kImage);
    MakeFile(games / "a" / "b" / "c" / "d" / "e" / "too-deep.rvz", kImage);
    MakeFile(games / "small.rvz", 1024);
    MakeFile(sd / "jp.rvz", kImage);
    const std::vector<LibraryFolder> folders{{games, true}, {sd, false}, {root / "unplugged", true},
                                             {games / "Europe", true}};
    ScanProgress progress;
    auto scan = ScanLibrary(folders, {}, false, {"R4QE01"}, {}, progress, probe, banner);
    Require(!scan.cancelled && !scan.limited, "A complete scan");
    Require(scan.available == std::vector<bool>{true, true, false, true}, "Folder availability");
    Require(progress.found == 9 && progress.checked == 9 && !progress.listing, "Progress counts every file");
    Require(scan.images.size() == 7, "Charged and unreadable images are listed");
    Require(scan.other_games == 1, "Other games are counted, not listed");
    Require(!opened.count("too-deep.rvz") && !opened.count("small.rvz"), "Depth and size limits");
    Require(opened["eu2.rvz"] == 1, "A folder inside another lists its images once");
    for (const auto& image : scan.images)
        if (image.path.filename() == "jp.rvz") Require(image.folder == 1, "Images know their folder");
    Require(banner_reads.count("R4QE01") == 0, "Known banners are not read again");
    Require(banner_reads["R4QP01"] == 1 && banner_reads["R4QK01"] == 1 && banner_reads["R4QJ01"] == 1,
            "One banner read per release");
    Require(scan.banners.size() == 2 && scan.banners.count("R4QP01") && scan.banner_failures.count("R4QJ01"),
            "Banners and failures are reported");

    // Order and search.
    auto arranged = ArrangeImages(scan.images, LibrarySort::Release, false, "");
    Require(Names(arranged) == "us copy.iso, us.rvz, eu1.iso, eu2.rvz, jp.rvz, deep.rvz, broken.rvz",
            "Release order: " + Names(arranged));
    arranged = ArrangeImages(scan.images, LibrarySort::Size, false, "");
    Require(PathUtf8(arranged.front()->path.filename()) == "eu1.iso", "Largest first");
    arranged = ArrangeImages(scan.images, LibrarySort::Name, true, "");
    Require(PathUtf8(arranged.front()->path.filename()) == "us.rvz", "Names descending");
    Require(Names(ArrangeImages(scan.images, LibrarySort::Release, false, "europe REV 2")) == "eu2.rvz",
            "Search words match region and revision");
    Require(Names(ArrangeImages(scan.images, LibrarySort::Release, false, "r4qj")) == "jp.rvz", "Search the ID");
    Require(Names(ArrangeImages(scan.images, LibrarySort::Release, false, "broken")) == "broken.rvz",
            "Search file names");
    Require(ArrangeImages(scan.images, LibrarySort::Release, false, "zzz").empty(), "No match");

    // States, titles and copies.
    for (const auto& image : scan.images)
    {
        const auto name = PathUtf8(image.path.filename());
        const auto state = StateOf(image);
        if (name == "jp.rvz" || name == "deep.rvz") Require(state == ImageState::Unsupported, name + " unsupported");
        else if (name == "broken.rvz") Require(state == ImageState::Unreadable, "broken.rvz unreadable");
        else Require(state == ImageState::Playable, name + " playable");
        if (name == "eu2.rvz") Require(ImageTitle(image) == "Mario Strikers Charged Football", "European title");
        if (name == "broken.rvz") Require(ImageTitle(image) == "broken.rvz", "Unreadable images show their name");
    }
    const auto copies = SameRelease(scan.images, "R4QE01", 1, games / "us.rvz");
    Require(copies.size() == 1 && copies[0]->path.filename() == "us copy.iso", "Copies of the same release");
    Require(SameRelease(scan.images, "R4QP01", 3, {}).empty(), "No copy of another revision");

    // A second scan opens only changed files; unreadable ones only when asked.
    opened.clear();
    banner_reads.clear();
    MakeFile(games / "us.rvz", kImage + 10);
    ScanProgress again;
    const std::set<std::string> known{"R4QE01", "R4QP01", "R4QK01", "R4QJ01"};
    auto rescan = ScanLibrary(folders, scan.cache, false, known, {}, again, probe, banner);
    Require(opened.size() == 1 && opened.count("us.rvz"), "Only the changed file is opened again");
    Require(banner_reads.empty(), "No banner is read again");
    opened.clear();
    ScanProgress recheck;
    rescan = ScanLibrary(folders, rescan.cache, true, known, {}, recheck, probe, banner);
    Require(opened.size() == 1 && opened.count("broken.rvz"), "A refresh opens unreadable files again");

    // The cache keeps files below an unplugged drive and pinned files, and
    // forgets files that are gone from available folders.
    auto cache = rescan.cache;
    cache.Put(root / "unplugged" / "x.rvz", kImage, 1, Charged("R4QE01", 1));
    cache.Put(root / "elsewhere" / "pinned.rvz", kImage, 1, Charged("R4QP01", 1));
    cache.Put(games / "deleted.rvz", kImage, 1, Charged("R4QP01", 1));
    ScanProgress keep;
    rescan = ScanLibrary(folders, cache, false, known, {root / "elsewhere" / "pinned.rvz"}, keep, probe, banner);
    Require(rescan.cache.Last(root / "unplugged" / "x.rvz"), "An unplugged drive's files stay known");
    Require(rescan.cache.Last(root / "elsewhere" / "pinned.rvz"), "Kept paths stay known");
    Require(!rescan.cache.Last(games / "deleted.rvz"), "Deleted files are forgotten");
    Require(rescan.cache.Last(games / "metroid.iso"), "Other games stay known (they are not opened again)");

    // A cancelled scan stops and leaves the cache as it was.
    ScanProgress cancelled;
    cancelled.cancel = true;
    rescan = ScanLibrary(folders, cache, false, known, {}, cancelled, probe, banner);
    Require(rescan.cancelled && rescan.images.empty() && rescan.cache.Last(games / "deleted.rvz"),
            "Cancelling stops the scan");

    // The cache file.
    const auto file = root / "data" / "launcher" / "library.json";
    Require(SaveImageCache(scan.cache, file), "The cache is saved");
    Require(LoadImageCache(file).size() == scan.cache.size(), "and read back");
    Require(LoadImageCache(root / "missing.json").size() == 0, "A missing cache file is an empty cache");

    // Folders of found images, and why an image is missing.
    auto found = FoldersOfImages({games / "us.rvz", games / "us copy.iso", sd / "jp.rvz"});
    Require(found.size() == 2 && found[0].path == games && !found[0].subfolders && found[1].path == sd,
            "Each image's own folder, once");
    found = FoldersOfImages({games / "Europe" / "eu2.rvz", games / "us.rvz", sd / "jp.rvz",
                             games / "a" / "b" / "c" / "d" / "e" / "far.rvz"});
    Require(found.size() == 3 && found[0].path == games && found[0].subfolders && found[1].path == sd
                && found[2].path == games / "a" / "b" / "c" / "d" / "e",
            "A folder inside another is left to the outer one, a few levels down");
    Require(MissingImageReason(games / "us.rvz").empty(), "An existing image is not missing");
    Require(MissingImageReason(games / "gone.rvz").find("no longer in") != std::string::npos, "A file that is gone");
    Require(MissingImageReason(root / "unplugged" / "x.rvz").find("connect it") != std::string::npos,
            "A folder that is gone");
    Require(FileSizeText(1503238554) == "1.40 GB" && FileSizeText(860ull << 20) == "860 MB", "File sizes");
    std::puts("Scan checks passed.");
}

const char* KindName(DiscProbe::Kind kind)
{
    switch (kind)
    {
    case DiscProbe::Kind::Charged: return "charged";
    case DiscProbe::Kind::OtherGame: return "other";
    case DiscProbe::Kind::NotDisc: return "notdisc";
    default: return "unreadable";
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "probe" && argc == 3)
        {
            const auto probe = ProbeDisc(PathFromUtf8(argv[2]));
            std::cout << KindName(probe.kind) << ' ' << probe.info.game_id << ' ' << unsigned(probe.info.revision)
                      << ' ' << probe.info.format << '\n';
            return 0;
        }
        if (mode == "banner" && argc == 5)
        {
            const auto rgba = ReadReleaseBanner(PathFromUtf8(argv[2]), argv[3]);
            std::ofstream(PathFromUtf8(argv[4]), std::ios::binary)
                .write(reinterpret_cast<const char*>(rgba.data()), std::streamsize(rgba.size()));
            return 0;
        }
        if (mode == "scan" && argc == 3)
        {
            ScanProgress progress;
            const auto scan = ScanLibrary({{PathFromUtf8(argv[2]), true}}, {}, false, {}, {}, progress, ProbeDisc,
                                          ReadReleaseBanner);
            for (const auto* image : ArrangeImages(scan.images, LibrarySort::Name, false, ""))
            {
                const char* state = StateOf(*image) == ImageState::Playable      ? "playable"
                                    : StateOf(*image) == ImageState::Unsupported ? "unsupported"
                                                                                 : "unreadable";
                std::cout << PathUtf8(image->path.filename()) << ' ' << state << ' ' << image->probe.info.game_id
                          << '\n';
            }
            std::cout << "other " << scan.other_games << '\n';
            for (const auto& [id, rgba] : scan.banners) std::cout << "banner " << id << ' ' << rgba.size() << '\n';
            for (const auto& id : scan.banner_failures) std::cout << "no-banner " << id << '\n';
            return 0;
        }
        if (argc != 1) return 2;
        const auto root = fs::temp_directory_path() / ("recharged-image-library-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        try
        {
            CheckBanners();
            CheckCache();
            CheckScan(root);
        }
        catch (...)
        {
            std::error_code ignored;
            fs::remove_all(root, ignored);
            throw;
        }
        fs::remove_all(root);
        std::puts("Image library checks passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "launcher_image_library: %s\n", error.what());
        return 1;
    }
}
