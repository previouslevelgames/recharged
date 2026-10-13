#include "runtime/fps_overlay.h"

#include "platform/path.h"

#include <aurora/imgui.h>
#include <imgui.h>
#include <misc/freetype/imgui_freetype.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace mscharged::runtime {
namespace {
// Digits and "FPS", plus the space and fallback characters.
constexpr ImWchar kGlyphs[] = {0x0020, 0x0020, 0x0030, 0x0039, 0x003F, 0x003F, 0x0046, 0x0046,
                               0x0050, 0x0050, 0x0053, 0x0053, 0};
constexpr ImU32 kPill = IM_COL32(0, 0, 0, 140);
constexpr ImU32 kEdge = IM_COL32(255, 255, 255, 28);
constexpr ImU32 kNumber = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kLabel = IM_COL32(255, 255, 255, 170);
} // namespace

FpsOverlay::FpsOverlay(const std::filesystem::path& font, OverlayCorner corner) : corner_(corner)
{
    std::ifstream input(font, std::ios::binary);
    if (input) fontData_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (fontData_.empty())
        std::fprintf(stderr, "Frame rate counter: font %s unavailable; using the built-in font.\n",
                     PathUtf8(font).c_str());
}

FpsOverlay::~FpsOverlay() { aurora_imgui_clear_overlay(); }

void FpsOverlay::SetRate(double rate)
{
    value_ = static_cast<int>(std::clamp(std::lround(rate), 0L, 999L));
}

void FpsOverlay::BuildFonts(const FpsOverlayFonts& sizes)
{
    ImGuiIO& io = ImGui::GetIO();
    for (;;) {
        io.Fonts->Clear();
        io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        const auto add = [&](float size) {
            config.SizePixels = size;
            if (fontData_.empty()) return io.Fonts->AddFontDefault(&config);
            return io.Fonts->AddFontFromMemoryTTF(fontData_.data(), static_cast<int>(fontData_.size()), size,
                                                  &config, kGlyphs);
        };
        number_ = add(sizes.number);
        label_ = add(sizes.label);
        if (io.Fonts->Build() || fontData_.empty()) break;
        std::fprintf(stderr, "Frame rate counter: unreadable font; using the built-in font.\n");
        fontData_.clear();
    }
    // Waits for presentations that still show the previous atlas.
    aurora_imgui_update_overlay_fonts();
    builtSize_ = sizes.number;
}

void FpsOverlay::Update(std::uint32_t surface_width, std::uint32_t surface_height)
{
    if (!value_ || surface_width == 0 || surface_height == 0) return;
    if (value_ == shownValue_ && surface_width == shownWidth_ && surface_height == shownHeight_) return;
    const auto fonts = FpsOverlayFontSizes(surface_width, surface_height);
    if (fonts.number != builtSize_) BuildFonts(fonts);
    char digits[8];
    std::snprintf(digits, sizeof digits, "%d", *value_);
    const ImFontGlyph* zero = number_->FindGlyph('0');
    FpsOverlayText text{};
    text.number_width = number_->CalcTextSizeA(fonts.number, FLT_MAX, 0.0f, digits).x;
    text.label_width = label_->CalcTextSizeA(fonts.label, FLT_MAX, 0.0f, "FPS").x;
    text.digit_top = zero ? zero->Y0 : 0.0f;
    text.digit_bottom = zero ? zero->Y1 : std::round(number_->Ascent);
    text.number_ascent = std::round(number_->Ascent);
    text.label_ascent = std::round(label_->Ascent);
    const auto layout = LayoutFpsOverlay(surface_width, surface_height, corner_, fonts, text);

    aurora_imgui_begin_overlay(surface_width, surface_height);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({layout.left, layout.top}, {layout.right, layout.bottom}, kPill, layout.rounding);
    draw->AddRect({layout.left + 0.5f, layout.top + 0.5f}, {layout.right - 0.5f, layout.bottom - 0.5f}, kEdge,
                  layout.rounding - 0.5f, 0, 1.0f);
    draw->AddText(number_, fonts.number, {layout.number_x, layout.number_y}, kNumber, digits);
    draw->AddText(label_, fonts.label, {layout.label_x, layout.label_y}, kLabel, "FPS");
    aurora_imgui_end_overlay();
    shownValue_ = value_;
    shownWidth_ = surface_width;
    shownHeight_ = surface_height;
}

} // namespace mscharged::runtime
