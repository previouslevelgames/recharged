#include "launcher/fallback_fonts.h"

#include "platform/name_glyphs.h"

#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace mscharged::launcher
{
namespace
{
struct SystemFont
{
    std::vector<char> data;
    int index = 0; // the face in a collection (.ttc)
};

// What a candidate file is read for: Japanese (kana and kanji), Korean
// (Hangul), both, or symbols.
enum Covers : unsigned
{
    kJapanese = 1,
    kKorean = 2,
    kSymbols = 4,
};

// The candidates, best first.
std::vector<std::pair<std::filesystem::path, unsigned>> Candidates()
{
    std::vector<std::pair<std::filesystem::path, unsigned>> files;
#if defined(_WIN32)
    const char* windows = std::getenv("WINDIR");
    const std::filesystem::path fonts = std::filesystem::path(windows ? windows : "C:\\Windows") / "Fonts";
    for (const char* name : {"YuGothM.ttc", "YuGothR.ttc", "meiryo.ttc", "msgothic.ttc"})
        files.emplace_back(fonts / name, kJapanese);
    files.emplace_back(fonts / "malgun.ttf", kKorean);
    files.emplace_back(fonts / "seguisym.ttf", kSymbols);
#elif defined(__APPLE__)
    for (const char* name : {"/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
                             "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc"})
        files.emplace_back(name, kJapanese);
    files.emplace_back("/System/Library/Fonts/AppleSDGothicNeo.ttc", kKorean);
    files.emplace_back("/System/Library/Fonts/Apple Symbols.ttf", kSymbols);
    files.emplace_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", kJapanese | kKorean | kSymbols);
#else
    for (const char* dir : {"/usr/share/fonts/noto-cjk", "/usr/share/fonts/opentype/noto",
                            "/usr/share/fonts/google-noto-cjk", "/usr/share/fonts/noto", "/usr/share/fonts/OTF"})
        for (const char* name : {"NotoSansCJK-Medium.ttc", "NotoSansCJK-Regular.ttc"})
            files.emplace_back(std::filesystem::path(dir) / name, kJapanese | kKorean);
    files.emplace_back("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", kJapanese | kKorean);
    files.emplace_back("/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", kJapanese | kKorean);
    for (const char* name : {"/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"})
        files.emplace_back(name, kSymbols);
#endif
    return files;
}

// The first Japanese and the first Korean font found (a CJK font is tens of
// megabytes: one each is enough), and every symbol font.
const std::vector<SystemFont>& SystemFonts()
{
    static const std::vector<SystemFont> fonts = [] {
        std::vector<SystemFont> found;
        unsigned covered = 0;
        for (const auto& [path, covers] : Candidates())
        {
            if (covers != kSymbols && (covers & ~covered & (kJapanese | kKorean)) == 0) continue;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) continue;
            std::ifstream file(path, std::ios::binary);
            std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (data.empty()) continue;
            found.push_back({std::move(data), 0});
            covered |= covers;
        }
        return found;
    }();
    return fonts;
}

ImVector<ImWchar> gRanges;
std::uint64_t gBuilt = 0;
} // namespace

void PrepareNameGlyphs()
{
    gBuilt = platform::NameGlyphsVersion();
    gRanges.clear();
    const auto glyphs = platform::NameGlyphs();
    if (glyphs.empty()) return;
    ImFontGlyphRangesBuilder builder;
    for (const auto c : glyphs)
        if (c <= 0xFFFF) builder.AddChar(static_cast<ImWchar>(c));
    builder.BuildRanges(&gRanges);
}

void MergeNameGlyphs(ImFontAtlas& atlas, float pixels)
{
    if (gRanges.empty()) return;
    for (const auto& font : SystemFonts())
    {
        ImFontConfig config;
        config.MergeMode = true;
        config.FontDataOwnedByAtlas = false;
        config.FontNo = font.index;
        config.OversampleH = 2;
        config.OversampleV = 1;
        config.PixelSnapH = false;
        atlas.AddFontFromMemoryTTF(const_cast<char*>(font.data.data()), static_cast<int>(font.data.size()), pixels,
                                   &config, gRanges.Data);
    }
}

std::uint64_t NameGlyphsBuilt() { return gBuilt; }
} // namespace mscharged::launcher
