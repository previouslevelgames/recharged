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
// The card shows players' names: Latin letters with accents, and dashes,
// quotes and the ellipsis.
constexpr ImWchar kCardGlyphs[] = {0x0020, 0x017F, 0x2010, 0x2027, 0};
// The card, in units of 1/720 of the picture's height: the launcher's colours.
constexpr ImU32 kDim = IM_COL32(4, 7, 12, 178);
constexpr ImU32 kCard = IM_COL32(18, 25, 41, 250);
constexpr ImU32 kCardBorder = IM_COL32(36, 48, 72, 255);
constexpr ImU32 kCardText = IM_COL32(234, 240, 250, 255);
constexpr ImU32 kCardMuted = IM_COL32(139, 151, 176, 255);
constexpr ImU32 kAccent = IM_COL32(76, 240, 122, 255);
constexpr ImU32 kAccentInk = IM_COL32(6, 33, 15, 255);
constexpr ImU32 kButton = IM_COL32(24, 33, 53, 255);
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

void HostOverlay::SetNetwork(const Network& network) { network_ = network; }

void HostOverlay::ShowMessage(std::string text, Clock::time_point now)
{
    message_ = std::move(text);
    messageShown_ = now;
}

void HostOverlay::SetCard(std::optional<Card> card) { card_ = std::move(card); }

void HostOverlay::BuildFonts(float size, float unit)
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
        const auto addCard = [&](float pixels) {
            config.SizePixels = pixels;
            if (fontData_.empty()) return io.Fonts->AddFontDefault(&config);
            return io.Fonts->AddFontFromMemoryTTF(fontData_.data(), static_cast<int>(fontData_.size()), pixels,
                                                  &config, kCardGlyphs);
        };
        label_ = add(size);
        messageFont_ = add(MessageFontSize(size));
        cardText_ = addCard(std::round(17.0f * unit));
        config.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_Bold;
        number_ = add(size);
        cardBold_ = addCard(std::round(26.0f * unit));
        if (io.Fonts->Build() || fontData_.empty()) break;
        std::fprintf(stderr, "Host overlay: unreadable font; using the built-in font.\n");
        fontData_.clear();
    }
    // Waits for presentations that still show the previous atlas.
    aurora_imgui_update_overlay_fonts();
    builtSize_ = size;
    builtUnit_ = unit;
}

void HostOverlay::DrawCard(float width, float height, float u)
{
    const Card& card = *card_;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({0, 0}, {width, height}, kDim);
    const float cardWidth = std::min(560.0f * u, width * 0.9f), pad = 28.0f * u;
    const float inner = cardWidth - 2.0f * pad;
    const float title = 26.0f * u, text = 17.0f * u, small = 14.0f * u, button = 46.0f * u;
    const auto measure = [&](ImFont* font, float size, const std::string& value) {
        return value.empty() ? 0.0f : font->CalcTextSizeA(size, FLT_MAX, inner, value.c_str()).y;
    };
    const float kicker = measure(cardBold_, small, card.kicker), heading = measure(cardBold_, title, card.title),
                detail = measure(cardText_, text, card.detail), note = measure(cardText_, small, card.note);
    const float cardHeight = pad + (kicker ? kicker + 6.0f * u : 0.0f) + heading + (detail ? 8.0f * u + detail : 0.0f)
        + 24.0f * u + button + (note ? 14.0f * u + note : 0.0f) + pad;
    const ImVec2 min{std::round((width - cardWidth) * 0.5f), std::round((height - cardHeight) * 0.5f)};
    const ImVec2 max{min.x + cardWidth, min.y + cardHeight};
    const float round = 18.0f * u;
    // A soft shadow, the card, its border and the accent line along its top.
    for (int n = 3; n >= 1; --n) {
        const float spread = float(n) * 6.0f * u;
        draw->AddRectFilled({min.x - spread, min.y - spread + 8.0f * u}, {max.x + spread, max.y + spread + 8.0f * u},
                            IM_COL32(0, 0, 0, 34), round + spread);
    }
    draw->AddRectFilled(min, max, kCard, round);
    draw->AddRect(min, max, kCardBorder, round, 0, std::max(1.0f, u));
    draw->AddRectFilled({min.x + round, min.y}, {max.x - round, min.y + std::max(2.0f, 3.0f * u)}, kAccent);

    float y = min.y + pad;
    const float x = min.x + pad;
    if (kicker) {
        draw->AddText(cardBold_, small, {x, y}, kAccent, card.kicker.c_str(), nullptr, inner);
        y += kicker + 6.0f * u;
    }
    draw->AddText(cardBold_, title, {x, y}, kCardText, card.title.c_str(), nullptr, inner);
    y += heading;
    if (detail) {
        y += 8.0f * u;
        draw->AddText(cardText_, text, {x, y}, kCardMuted, card.detail.c_str(), nullptr, inner);
        y += detail;
    }
    y += 24.0f * u;

    // The two answers, each with its button's letter in a circle.
    const float gap = 12.0f * u, half = (inner - gap) * 0.5f, label = 18.0f * u;
    const auto answer = [&](float left, const char* letter, const std::string& value, bool primary) {
        const ImVec2 a{left, y}, b{left + half, y + button};
        if (primary) draw->AddRectFilled(a, b, kAccent, button * 0.5f);
        else {
            draw->AddRectFilled(a, b, kButton, button * 0.5f);
            draw->AddRect(a, b, kCardBorder, button * 0.5f, 0, std::max(1.0f, u));
        }
        const float badge = 13.0f * u;
        const ImVec2 labelSize = cardBold_->CalcTextSizeA(label, FLT_MAX, 0, value.c_str());
        const float total = badge * 2.0f + 10.0f * u + labelSize.x;
        const float start = left + (half - total) * 0.5f, middle = y + button * 0.5f;
        const ImVec2 centre{start + badge, middle};
        draw->AddCircleFilled(centre, badge, primary ? kAccentInk : IM_COL32(52, 66, 94, 255));
        const ImVec2 letterSize = cardBold_->CalcTextSizeA(label * 0.8f, FLT_MAX, 0, letter);
        draw->AddText(cardBold_, label * 0.8f, {centre.x - letterSize.x * 0.5f, centre.y - letterSize.y * 0.5f},
                      primary ? kAccent : kCardText, letter);
        draw->AddText(cardBold_, label, {start + badge * 2.0f + 10.0f * u, middle - labelSize.y * 0.5f},
                      primary ? kAccentInk : kCardText, value.c_str());
    };
    answer(x, "A", card.accept, true);
    answer(x + half + gap, "B", card.decline, false);
    y += button;
    if (note) {
        y += 14.0f * u;
        draw->AddText(cardText_, small, {x, y}, kCardMuted, card.note.c_str(), nullptr, inner);
    }
}

void HostOverlay::Update(std::uint32_t surface_width, std::uint32_t surface_height, Clock::time_point now)
{
    if (surface_width == 0 || surface_height == 0) return;
    std::array<std::optional<int>, kMaxPerformanceMetrics> values{};
    // FPS, GPU and CPU; LAT in a network session; DLY and RB with rollback.
    std::size_t count = 0;
    if (usage_) {
        const auto usage = usage_->Latest();
        values = {value_, OverlayNumber(usage.gpu_percent, 100), OverlayNumber(usage.cpu_percent, 100),
                  OverlayNumber(network_.latency_ms, 999), OverlayNumber(network_.delay_ms, 999),
                  OverlayNumber(network_.rollback_frames, 99)};
        count = network_.session ? (network_.rollback ? 6 : 4) : 3;
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
    const bool showing = performance_ || opacity > 0 || card_;
    if (!showing) {
        if (drawn_) aurora_imgui_clear_overlay();
        drawn_ = false;
        return;
    }
    if (drawn_ && values == shownValues_ && count == shownCount_ && message_ == shownMessage_ && opacity == shownOpacity_
        && card_ == shownCard_ && surface_width == shownWidth_ && surface_height == shownHeight_)
        return;
    const auto size = FpsOverlayFontSize(surface_width, surface_height);
    const float unit = std::clamp(float(surface_height) / 720.0f, 0.5f, 4.0f);
    if (size != builtSize_ || unit != builtUnit_) BuildFonts(size, unit);
    aurora_imgui_begin_overlay(surface_width, surface_height);
    // The card first: the performance numbers and messages stay on top.
    if (card_) DrawCard(float(surface_width), float(surface_height), unit);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float shadow = std::max(1.0f, std::round(size / 15.0f));
    std::optional<FpsOverlayLayout> performanceLayout;
    if (performance_) {
        std::array<std::array<char, 8>, kMaxPerformanceMetrics> strings{};
        std::array<FpsOverlayText, kMaxPerformanceMetrics> text{};
        for (std::size_t n = 0; n < count; ++n) {
            const bool percent = n == 1 || n == 2;
            if (values[n]) std::snprintf(strings[n].data(), strings[n].size(), percent ? "%d%%" : "%d", *values[n]);
            else std::snprintf(strings[n].data(), strings[n].size(), "N/A");
            const auto measure = [&](const char* value) { return number_->CalcTextSizeA(size, FLT_MAX, 0, value).x; };
            text[n] = {label_->CalcTextSizeA(size, FLT_MAX, 0, kPerformanceLabels[n]).x, measure(strings[n].data()),
                       std::max(measure(percent ? "100%" : n == 5 ? "99" : "999"), measure("N/A"))};
        }
        const auto layout = LayoutFpsOverlay(surface_width, surface_height, *performance_, size, text.data(), count);
        const auto drawText = [&](ImFont* font, float x, ImU32 color, const char* value) {
            draw->AddText(font, size, {x + shadow, layout.text_y + shadow}, kShadow, value);
            draw->AddText(font, size, {x, layout.text_y}, color, value);
        };
        for (std::size_t n = 0; n < layout.count; ++n) {
            drawText(label_, layout.label_x[n], kLabel, kPerformanceLabels[layout.metric[n]]);
            drawText(number_, layout.value_x[n], kNumber, strings[layout.metric[n]].data());
        }
        for (std::size_t n = 0; n + 1 < layout.count; ++n) {
            const float x = layout.separator_x[n];
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
    shownCount_ = count;
    shownMessage_ = message_;
    shownCard_ = card_;
    shownOpacity_ = opacity;
    shownWidth_ = surface_width;
    shownHeight_ = surface_height;
}

} // namespace mscharged::runtime
