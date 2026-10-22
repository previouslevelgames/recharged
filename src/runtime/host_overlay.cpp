#include "runtime/host_overlay.h"

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
// Printable ASCII and the multiplication sign of sizes (1920 × 1080).
constexpr ImWchar kGlyphs[] = {0x0020, 0x007E, 0x00D7, 0x00D7, 0};
constexpr ImU32 kNumber = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kLabel = IM_COL32(211, 215, 221, 255);
constexpr ImU32 kDivider = IM_COL32(211, 215, 221, 190);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);
// A message stays this long, then fades out over the last part.
constexpr auto kMessageTime = std::chrono::milliseconds(2500);
constexpr auto kMessageFade = std::chrono::milliseconds(500);
// Fade steps: the overlay is rebuilt once per step, not every frame.
constexpr int kOpacitySteps = 8;

ImU32 WithAlpha(ImU32 color, int opacity)
{
    const auto alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    return (color & ~IM_COL32_A_MASK) | (ImU32(alpha * opacity / kOpacitySteps) << IM_COL32_A_SHIFT);
}
} // namespace

HostOverlay::HostOverlay(const std::filesystem::path& font, std::optional<OverlayCorner> performance)
    : performance_(performance)
{
    if (performance_) usage_.emplace();
    std::ifstream input(font, std::ios::binary);
    if (input) fontData_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (fontData_.empty())
        std::fprintf(stderr, "Host overlay: font %s unavailable; using the built-in font.\n", PathUtf8(font).c_str());
}

HostOverlay::~HostOverlay() { aurora_imgui_clear_overlay(); }

void HostOverlay::SetRate(double rate) { value_ = OverlayNumber(rate, 999); }

void HostOverlay::ShowMessage(std::string text, Clock::time_point now)
{
    message_ = std::move(text);
    messageShown_ = now;
}

void HostOverlay::BuildFonts(float size)
{
    ImGuiIO& io = ImGui::GetIO();
    for (;;) {
        io.Fonts->Clear();
        io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        const auto add = [&](float pixels) {
            config.SizePixels = pixels;
            if (fontData_.empty()) return io.Fonts->AddFontDefault(&config);
            return io.Fonts->AddFontFromMemoryTTF(fontData_.data(), static_cast<int>(fontData_.size()), pixels,
                                                  &config, kGlyphs);
        };
        label_ = add(size);
        messageFont_ = add(MessageFontSize(size));
        config.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_Bold;
        number_ = add(size);
        if (io.Fonts->Build() || fontData_.empty()) break;
        std::fprintf(stderr, "Host overlay: unreadable font; using the built-in font.\n");
        fontData_.clear();
    }
    // Waits for presentations that still show the previous atlas.
    aurora_imgui_update_overlay_fonts();
    builtSize_ = size;
}

void HostOverlay::Update(std::uint32_t surface_width, std::uint32_t surface_height, Clock::time_point now)
{
    if (surface_width == 0 || surface_height == 0) return;
    std::array<std::optional<int>, 3> values{};
    if (usage_) {
        const auto usage = usage_->Latest();
        values = {value_, OverlayNumber(usage.gpu_percent, 100), OverlayNumber(usage.cpu_percent, 100)};
    }
    // The message's opacity in steps: full, then fading out, then gone.
    int opacity = 0;
    if (!message_.empty()) {
        const auto age = now - messageShown_;
        if (age < kMessageTime - kMessageFade) opacity = kOpacitySteps;
        else if (age < kMessageTime) {
            const double left = std::chrono::duration<double, std::milli>(kMessageTime - age).count();
            opacity = std::min(kOpacitySteps, int(std::ceil(left * kOpacitySteps / double(kMessageFade.count()))));
        }
        if (opacity == 0) message_.clear();
    }
    const bool showing = performance_ || opacity > 0;
    if (!showing) {
        if (drawn_) aurora_imgui_clear_overlay();
        drawn_ = false;
        return;
    }
    if (drawn_ && values == shownValues_ && message_ == shownMessage_ && opacity == shownOpacity_
        && surface_width == shownWidth_ && surface_height == shownHeight_)
        return;
    const auto size = FpsOverlayFontSize(surface_width, surface_height);
    if (size != builtSize_) BuildFonts(size);
    aurora_imgui_begin_overlay(surface_width, surface_height);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float shadow = std::max(1.0f, std::round(size / 15.0f));
    std::optional<FpsOverlayLayout> performanceLayout;
    if (performance_) {
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
        const auto layout = LayoutFpsOverlay(surface_width, surface_height, *performance_, size, text);
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
        performanceLayout = layout;
    }
    if (opacity > 0) {
        const float messageSize = MessageFontSize(size);
        const float width = messageFont_->CalcTextSizeA(messageSize, FLT_MAX, 0, message_.c_str()).x;
        const bool belowPerformance = performance_ && *performance_ == OverlayCorner::TopLeft;
        const auto layout = LayoutOverlayMessage(surface_width, surface_height, messageSize, width,
                                                 belowPerformance ? &*performanceLayout : nullptr);
        draw->AddRectFilled({layout.left, layout.top}, {layout.right, layout.bottom},
                            WithAlpha(IM_COL32(16, 18, 22, 190), opacity), std::round(messageSize * 0.35f));
        draw->AddText(messageFont_, messageSize, {layout.text_x, layout.text_y}, WithAlpha(kNumber, opacity),
                      message_.c_str());
    }
    aurora_imgui_end_overlay();
    drawn_ = true;
    shownValues_ = values;
    shownMessage_ = message_;
    shownOpacity_ = opacity;
    shownWidth_ = surface_width;
    shownHeight_ = surface_height;
}

} // namespace mscharged::runtime
