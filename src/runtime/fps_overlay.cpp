#include "runtime/fps_overlay.h"

#include "platform/path.h"

#include <aurora/imgui.h>
#include <imgui.h>
#include <misc/freetype/imgui_freetype.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace mscharged::runtime {
namespace {
// Digits, percent, slash, N/A and the four metric labels.
constexpr ImWchar kGlyphs[] = {0x0020, 0x0055, 0};
constexpr ImU32 kNumber = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kLabel = IM_COL32(211, 215, 221, 255);
constexpr ImU32 kDivider = IM_COL32(211, 215, 221, 190);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);
} // namespace

FpsOverlay::FpsOverlay(const std::filesystem::path& font, OverlayCorner corner) : corner_(corner)
{
    std::ifstream input(font, std::ios::binary);
    if (input) fontData_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (fontData_.empty())
        std::fprintf(stderr, "Performance overlay: font %s unavailable; using the built-in font.\n",
                     PathUtf8(font).c_str());
}

FpsOverlay::~FpsOverlay() { aurora_imgui_clear_overlay(); }

void FpsOverlay::SetRate(double rate) { value_ = OverlayNumber(rate, 999); }

void FpsOverlay::BuildFonts(float size)
{
    ImGuiIO& io = ImGui::GetIO();
    for (;;) {
        io.Fonts->Clear();
        io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.SizePixels = size;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        const auto add = [&] {
            if (fontData_.empty()) return io.Fonts->AddFontDefault(&config);
            return io.Fonts->AddFontFromMemoryTTF(fontData_.data(), static_cast<int>(fontData_.size()), size,
                                                  &config, kGlyphs);
        };
        label_ = add();
        config.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_Bold;
        number_ = add();
        if (io.Fonts->Build() || fontData_.empty()) break;
        std::fprintf(stderr, "Performance overlay: unreadable font; using the built-in font.\n");
        fontData_.clear();
    }
    // Waits for presentations that still show the previous atlas.
    aurora_imgui_update_overlay_fonts();
    builtSize_ = size;
}

void FpsOverlay::Update(std::uint32_t surface_width, std::uint32_t surface_height)
{
    if (surface_width == 0 || surface_height == 0) return;
    const auto usage = usage_.Latest();
    const std::array<std::optional<int>, 3> values = {
        value_, OverlayNumber(usage.gpu_percent, 100), OverlayNumber(usage.cpu_percent, 100)};
    if (values == shownValues_ && surface_width == shownWidth_ && surface_height == shownHeight_) return;
    const auto size = FpsOverlayFontSize(surface_width, surface_height);
    if (size != builtSize_) BuildFonts(size);
    std::array<std::array<char, 8>, 4> strings{};
    std::array<FpsOverlayText, 4> text{};
    for (std::size_t n = 0; n < strings.size(); ++n) {
        if (n < values.size() && values[n])
            std::snprintf(strings[n].data(), strings[n].size(), n == 0 ? "%d" : "%d%%", *values[n]);
        else std::snprintf(strings[n].data(), strings[n].size(), "N/A");
        const auto measure = [&](const char* value) { return number_->CalcTextSizeA(size, FLT_MAX, 0, value).x; };
        text[n] = {label_->CalcTextSizeA(size, FLT_MAX, 0, kPerformanceLabels[n]).x, measure(strings[n].data()),
                   std::max(measure(n == 0 ? "999" : n == 3 ? "N/A" : "100%"), measure("N/A"))};
    }
    const auto layout = LayoutFpsOverlay(surface_width, surface_height, corner_, size, text);
    aurora_imgui_begin_overlay(surface_width, surface_height);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float shadow = std::max(1.0f, std::round(size / 15.0f));
    const auto drawText = [&](ImFont* font, float x, ImU32 color, const char* value) {
        draw->AddText(font, size, {x + shadow, layout.text_y + shadow}, kShadow, value);
        draw->AddText(font, size, {x, layout.text_y}, color, value);
    };
    for (std::size_t n = 0; n < strings.size(); ++n) {
        drawText(label_, layout.label_x[n], kLabel, kPerformanceLabels[n]);
        drawText(number_, layout.value_x[n], kNumber, strings[n].data());
    }
    for (const float x : layout.separator_x) {
        draw->AddLine({x + shadow, layout.top + shadow}, {x + shadow, layout.bottom + shadow}, kShadow, shadow);
        draw->AddLine({x, layout.top}, {x, layout.bottom}, kDivider, shadow);
    }
    aurora_imgui_end_overlay();
    shownValues_ = values;
    shownWidth_ = surface_width;
    shownHeight_ = surface_height;
}

} // namespace mscharged::runtime
