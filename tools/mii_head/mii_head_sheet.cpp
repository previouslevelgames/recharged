// Developer tool for src/platform/mii_head.h: contact sheets of Mii heads,
// timings, and comparisons with the game's own icons. See Usage() below.
#include "platform/mii_head.h"
#include "platform/mii_maker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace mscharged::platform;
using namespace mscharged::platform::mii;

void Usage()
{
    std::cout <<
        R"(mii_head_sheet --parts IMAGE|RFLRes01.arc|RFL_Res.dat [what] [how] [output]

What (any mix, in this order):
  --db RFL_DB.dat        every Mii of a Mii database (repeatable)
  --records FILE         records of 74 bytes each, back to back
  --random N             N random Miis (MakeRandomMii), seeds from --seed (1)
How:
  --size N               head size in pixels (128)
  --background B         transparent (default), favorite or RRGGBB
  --8bit                 8 bits per channel instead of the Wii's precision
  --expression E         normal smile anger sorrow surprise blink open
Output:
  --out SHEET.png        contact sheet (on a checkerboard), --columns N (10)
  --each DIR             one PNG per head
  --time                 milliseconds per head
  --reference DIR        compare with the game's icons: DIR/records.bin and
                         DIR/icons/<index>.rgb5a3 (128 x 128 GX RGB5A3); the
                         sheet then shows game | ours | difference x4 per Mii
)";
}

std::vector<std::uint8_t> ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

// ---------------------------------------------------------------- PNG

std::uint32_t Crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// An RGBA PNG with stored (uncompressed) deflate blocks: enough for a tool.
void WritePng(const fs::path& path, const RgbaImage& image)
{
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const auto* row = image.pixels.data() + std::size_t(y) * image.width * 4;
        raw.insert(raw.end(), row, row + std::size_t(image.width) * 4);
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(std::uint8_t(n)), z.push_back(std::uint8_t(n >> 8));
        z.push_back(std::uint8_t(~n)), z.push_back(std::uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
        at += n;
        if (n == 0) break;
    }
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) a = (a + byte) % 65521, b = (b + a) % 65521;
    const std::uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(std::uint8_t(adler >> s));

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot write " + path.string());
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        std::vector<std::uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        const std::uint32_t length = std::uint32_t(data.size()), crc = Crc32(body.data(), body.size());
        const std::uint8_t len[4] = {std::uint8_t(length >> 24), std::uint8_t(length >> 16), std::uint8_t(length >> 8),
                                     std::uint8_t(length)};
        const std::uint8_t sum[4] = {std::uint8_t(crc >> 24), std::uint8_t(crc >> 16), std::uint8_t(crc >> 8),
                                     std::uint8_t(crc)};
        out.write(reinterpret_cast<const char*>(len), 4);
        out.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
        out.write(reinterpret_cast<const char*>(sum), 4);
    };
    out.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<std::uint8_t> header(13);
    for (int s = 0; s < 4; ++s) header[std::size_t(s)] = std::uint8_t(image.width >> (24 - 8 * s));
    for (int s = 0; s < 4; ++s) header[std::size_t(4 + s)] = std::uint8_t(image.height >> (24 - 8 * s));
    header[8] = 8, header[9] = 6;
    chunk("IHDR", header);
    chunk("IDAT", z);
    chunk("IEND", {});
}

// ---------------------------------------------------------------- Miis

struct Mii {
    std::string name;
    std::string record;
};

std::string Utf8Name(const std::string& record)
{
    std::string text;
    for (int n = 0; n < 10; ++n) {
        const unsigned c = (std::uint8_t(record[std::size_t(2 + 2 * n)]) << 8) | std::uint8_t(record[std::size_t(3 + 2 * n)]);
        if (!c) break;
        if (c < 0x80) text += char(c);
        else if (c < 0x800) text += {char(0xC0 | (c >> 6)), char(0x80 | (c & 0x3F))};
        else text += {char(0xE0 | (c >> 12)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F))};
    }
    return text;
}

std::vector<Mii> DatabaseMiis(const fs::path& path)
{
    const auto bytes = ReadFile(path);
    if (bytes.size() < 4 + 100 * kMiiRecordBytes || std::memcmp(bytes.data(), "RNOD", 4) != 0)
        throw std::runtime_error(path.string() + " is not a Mii database.");
    std::vector<Mii> miis;
    for (std::size_t slot = 0; slot < 100; ++slot) {
        std::string record(reinterpret_cast<const char*>(bytes.data()) + 4 + slot * kMiiRecordBytes, kMiiRecordBytes);
        if (std::all_of(record.begin() + 0x18, record.begin() + 0x20, [](char c) { return c == 0; })) continue;
        miis.push_back({Utf8Name(record), record});
    }
    return miis;
}

MiiParts LoadParts(const fs::path& path)
{
    const auto bytes = ReadFile(path);
    if (bytes.size() >= 4 && bytes[0] == 0x55 && bytes[1] == 0xAA && bytes[2] == 0x38 && bytes[3] == 0x2D)
        return MiiParts::FromArchive(bytes);
    if (bytes.size() >= 2 && bytes[0] == 0 && bytes[1] == 18) return MiiParts(bytes);
    return ReadMiiPartsFromImage(path);
}

// ---------------------------------------------------------------- images

RgbaImage Blank(int width, int height)
{
    RgbaImage image;
    image.width = width, image.height = height;
    image.pixels.assign(std::size_t(width) * height * 4, 0);
    return image;
}

// Draw `head` into `sheet` at (x, y) over a checkerboard.
void Place(RgbaImage& sheet, const RgbaImage& head, int x0, int y0)
{
    for (int y = 0; y < head.height; ++y)
        for (int x = 0; x < head.width; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (sx < 0 || sy < 0 || sx >= sheet.width || sy >= sheet.height) continue;
            const std::uint8_t* src = head.pixels.data() + (std::size_t(y) * head.width + x) * 4;
            std::uint8_t* dst = sheet.pixels.data() + (std::size_t(sy) * sheet.width + sx) * 4;
            const int check = ((x / 8 + y / 8) & 1) ? 200 : 232;
            for (int k = 0; k < 3; ++k) dst[k] = std::uint8_t((src[k] * src[3] + check * (255 - src[3]) + 127) / 255);
            dst[3] = 255;
        }
}

// The game's icon: 128 x 128 RGB5A3 in 4 x 4 tiles, as straight RGBA like
// RenderMiiHead's transparent heads.
RgbaImage DecodeGameIcon(const std::vector<std::uint8_t>& raw, int size)
{
    if (raw.size() < std::size_t(size) * size * 2) throw std::runtime_error("A game icon is cut short.");
    RgbaImage image = Blank(size, size);
    const auto expand = [](int v, int bits) {
        return bits == 5 ? (v << 3) | (v >> 2) : bits == 4 ? (v << 4) | v : (v << 5) | (v << 2) | (v >> 1);
    };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const std::size_t tile = std::size_t(y / 4) * (size / 4) + x / 4, at = tile * 32 + ((y % 4) * 4 + x % 4) * 2;
            const unsigned v = (raw[at] << 8) | raw[at + 1];
            int r, g, b, a;
            if (v & 0x8000) r = expand((v >> 10) & 31, 5), g = expand((v >> 5) & 31, 5), b = expand(v & 31, 5), a = 255;
            else r = expand((v >> 8) & 15, 4), g = expand((v >> 4) & 15, 4), b = expand(v & 15, 4), a = expand(v >> 12, 3);
            std::uint8_t* out = image.pixels.data() + (std::size_t(y) * size + x) * 4;
            if (a == 0) continue;
            const int rgb[3] = {r, g, b};
            for (int k = 0; k < 3; ++k) out[k] = std::uint8_t(std::min(255, (rgb[k] * 255 + a / 2) / a));
            out[3] = std::uint8_t(a);
        }
    return image;
}

struct Difference {
    int alpha_mismatch = 0;  // pixels transparent in one, opaque in the other
    int compared = 0;        // pixels with alpha in both
    int over_8 = 0;          // of those, a channel more than 8 off
    double mean = 0;         // mean absolute channel difference
    int max = 0;
};

Difference Compare(const RgbaImage& game, const RgbaImage& ours, RgbaImage* picture)
{
    Difference d;
    double sum = 0;
    if (picture) *picture = Blank(game.width, game.height);
    for (std::size_t n = 0; n < std::size_t(game.width) * game.height; ++n) {
        const std::uint8_t* a = game.pixels.data() + n * 4;
        const std::uint8_t* b = ours.pixels.data() + n * 4;
        std::uint8_t* p = picture ? picture->pixels.data() + n * 4 : nullptr;
        if ((a[3] == 0) != (b[3] == 0)) {
            ++d.alpha_mismatch;
            if (p) p[0] = 255, p[1] = 0, p[2] = 255, p[3] = 255;
            continue;
        }
        if (a[3] == 0) continue;
        ++d.compared;
        int worst = std::abs(a[3] - b[3]);
        for (int k = 0; k < 3; ++k) {
            const int diff = std::abs(a[k] - b[k]);
            sum += diff;
            worst = std::max(worst, diff);
            if (p) p[k] = std::uint8_t(std::min(255, diff * 4));
        }
        if (p) p[3] = 255;
        d.max = std::max(d.max, worst);
        if (worst > 8) ++d.over_8;
    }
    d.mean = d.compared ? sum / (3.0 * d.compared) : 0;
    return d;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        fs::path parts_path, out, each, reference;
        std::vector<fs::path> databases, record_files;
        int random = 0, seed = 1, columns = 10;
        bool time = false;
        MiiHeadOptions options;
        for (int n = 1; n < argc; ++n) {
            const std::string arg = argv[n];
            const auto value = [&]() -> std::string {
                if (n + 1 >= argc) throw std::runtime_error(arg + " needs a value.");
                return argv[++n];
            };
            if (arg == "--help" || arg == "-h") return Usage(), 0;
            else if (arg == "--parts") parts_path = value();
            else if (arg == "--db") databases.push_back(value());
            else if (arg == "--records") record_files.push_back(value());
            else if (arg == "--random") random = std::stoi(value());
            else if (arg == "--seed") seed = std::stoi(value());
            else if (arg == "--size") options.width = options.height = std::stoi(value());
            else if (arg == "--columns") columns = std::max(1, std::stoi(value()));
            else if (arg == "--8bit") options.wii_precision = false;
            else if (arg == "--out") out = value();
            else if (arg == "--each") each = value();
            else if (arg == "--time") time = true;
            else if (arg == "--reference") reference = value();
            else if (arg == "--background") {
                const std::string b = value();
                if (b == "transparent") options.background = MiiHeadOptions::Background::Transparent;
                else if (b == "favorite") options.background = MiiHeadOptions::Background::Favorite;
                else {
                    const unsigned long rgb = std::stoul(b, nullptr, 16);
                    options.background = MiiHeadOptions::Background::Color;
                    options.background_color = {std::uint8_t(rgb >> 16), std::uint8_t(rgb >> 8), std::uint8_t(rgb)};
                }
            } else if (arg == "--expression") {
                const std::string e = value();
                const char* names[] = {"normal", "smile", "anger", "sorrow", "surprise", "blink", "open"};
                const auto found = std::find_if(std::begin(names), std::end(names), [&](const char* s) { return e == s; });
                if (found == std::end(names)) throw std::runtime_error("Unknown expression " + e);
                options.expression = Expression(found - std::begin(names));
            } else throw std::runtime_error("Unknown option " + arg + " (--help)");
        }
        if (parts_path.empty()) return Usage(), 2;
        const MiiParts parts = LoadParts(parts_path);

        std::vector<Mii> miis;
        for (const auto& db : databases)
            for (auto& mii : DatabaseMiis(db)) miis.push_back(std::move(mii));
        for (const auto& file : record_files) {
            const auto bytes = ReadFile(file);
            for (std::size_t at = 0; at + kMiiRecordBytes <= bytes.size(); at += kMiiRecordBytes) {
                std::string record(reinterpret_cast<const char*>(bytes.data()) + at, kMiiRecordBytes);
                miis.push_back({Utf8Name(record), record});
            }
        }
        for (int n = 0; n < random; ++n)
            miis.push_back({"Random " + std::to_string(seed + n),
                            MakeRandomMii("Random", std::uint32_t(seed + n), 1700000000 + n)});

        if (!reference.empty()) {
            const auto bytes = ReadFile(reference / "records.bin");
            options = MiiHeadOptions{};
            const int size = 128;
            std::vector<int> present;
            for (int i = 0; i < int(bytes.size() / kMiiRecordBytes); ++i)
                if (fs::exists(reference / "icons" / (std::to_string(i) + ".rgb5a3"))) present.push_back(i);
            if (present.empty()) throw std::runtime_error("No game icons in " + reference.string());
            const int count = int(present.size()), per_row = std::min(columns, count);
            RgbaImage sheet = Blank(per_row * (3 * size + 16), ((count + per_row - 1) / per_row) * (size + 8));
            Difference total;
            double mean_sum = 0;
            int worst_index = -1, exact = 0;
            for (int n = 0; n < count; ++n) {
                const int i = present[std::size_t(n)];
                const std::string record(reinterpret_cast<const char*>(bytes.data()) + std::size_t(i) * kMiiRecordBytes,
                                         kMiiRecordBytes);
                const RgbaImage game =
                    DecodeGameIcon(ReadFile(reference / "icons" / (std::to_string(i) + ".rgb5a3")), size);
                const RgbaImage ours = RenderMiiHead(parts, record, options);
                RgbaImage picture;
                const Difference d = Compare(game, ours, &picture);
                exact += game.pixels == ours.pixels;
                std::printf("%3d %-12s alpha-mismatch %4d  compared %5d  mean %.3f  max %3d  over8 %4d\n", i,
                            Utf8Name(record).c_str(), d.alpha_mismatch, d.compared, d.mean, d.max, d.over_8);
                total.alpha_mismatch += d.alpha_mismatch;
                total.compared += d.compared;
                total.over_8 += d.over_8;
                mean_sum += d.mean * d.compared;
                if (d.max > total.max) total.max = d.max, worst_index = i;
                const int x = (n % per_row) * (3 * size + 16), y = (n / per_row) * (size + 8);
                Place(sheet, game, x, y);
                Place(sheet, ours, x + size, y);
                Place(sheet, picture, x + 2 * size, y);
            }
            std::printf("total: %d icons, %d identical; alpha-mismatch %d of %d pixels; head pixels %d: mean %.4f, "
                        "max %d (Mii %d), over8 %d\n",
                        count, exact, total.alpha_mismatch, count * size * size, total.compared,
                        total.compared ? mean_sum / total.compared : 0.0, total.max, worst_index, total.over_8);
            if (!out.empty()) WritePng(out, sheet);
            return 0;
        }

        if (miis.empty()) throw std::runtime_error("No Miis: give --db, --records or --random.");
        std::vector<RgbaImage> heads;
        const auto start = std::chrono::steady_clock::now();
        for (const auto& mii : miis) heads.push_back(RenderMiiHead(parts, mii.record, options));
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (time)
            std::printf("%zu heads of %d px: %.2f ms each\n", heads.size(), options.width,
                        1000.0 * seconds / double(heads.size()));
        for (std::size_t n = 0; n < miis.size(); ++n) std::printf("%3zu %s\n", n, miis[n].name.c_str());
        if (!each.empty()) {
            fs::create_directories(each);
            for (std::size_t n = 0; n < heads.size(); ++n) WritePng(each / (std::to_string(n) + ".png"), heads[n]);
        }
        if (!out.empty()) {
            const int count = int(heads.size()), per_row = std::min(columns, count);
            const int rows = (count + per_row - 1) / per_row, cell_w = options.width + 8, cell_h = options.height + 8;
            RgbaImage sheet = Blank(per_row * cell_w, rows * cell_h);
            for (int n = 0; n < count; ++n) Place(sheet, heads[std::size_t(n)], (n % per_row) * cell_w + 4, (n / per_row) * cell_h + 4);
            WritePng(out, sheet);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "mii_head_sheet: %s\n", error.what());
        return 1;
    }
}
