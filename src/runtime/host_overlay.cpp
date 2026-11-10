#include "runtime/host_overlay.h"

#include "launcher/fallback_fonts.h"
#include "platform/name_glyphs.h"
#include "platform/path.h"
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
#include "runtime/host_menus.h"
#endif

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
// Messages and the menus show players' names and Recharged's texts: what the
// font has for them (Latin letters with accents, Greek, Cyrillic), the general
// punctuation, currency and letterlike symbols; other characters come from
// the system's fonts.
constexpr ImWchar kTextGlyphs[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x2000, 0x206F,
                                   0x20A0, 0x20CF, 0x2100, 0x214F, 0};
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

// A plate in the look of Recharged's menus for a message or the hint: dark
// glass with a soft shadow and a gold edge at its left.
void Plate(ImDrawList* draw, ImVec2 min, ImVec2 max, float size, int opacity)
{
    const float round = std::round(size * 0.3f), edge = std::max(2.0f, std::round(size * 0.16f));
    const float drop = std::max(1.0f, std::round(size * 0.12f));
    draw->AddRectFilled({min.x, min.y + drop}, {max.x, max.y + drop}, WithAlpha(IM_COL32(0, 0, 0, 110), opacity), round);
    draw->AddRectFilled(min, max, WithAlpha(IM_COL32(18, 20, 26, 215), opacity), round);
    draw->AddRect(min, max, WithAlpha(IM_COL32(255, 255, 255, 34), opacity), round, 0, 1.0f);
    draw->AddRectFilled(min, {min.x + edge, max.y}, WithAlpha(IM_COL32(255, 200, 60, 255), opacity), round,
                        ImDrawFlags_RoundCornersLeft);
}

// The width of a key's glyph (KeyGlyph) at text size `size`.
float KeyGlyphWidth(ImFont* font, float size, const std::string& key)
{
    const float height = std::round(size * 1.45f);
    if (key == "HOME") return height;
    return std::max(height, std::round(font->CalcTextSizeA(size * 0.82f, FLT_MAX, 0, key.c_str()).x + size * 0.9f));
}

// A key as the player presses it, left-aligned at `at` and centred on
// `middle`: the Wii Remote's round HOME button with its house; else a key of
// the keyboard, with depth, its name on its top face.
void KeyGlyph(ImDrawList* draw, ImFont* font, float size, float x, float middle, const std::string& key)
{
    const float height = std::round(size * 1.45f), width = KeyGlyphWidth(font, size, key);
    const float top = std::round(middle - height * 0.5f), bottom = top + height;
    if (key == "HOME")
    {
        const ImVec2 centre{x + height * 0.5f, middle};
        const float radius = height * 0.5f;
        draw->AddCircleFilled({centre.x, centre.y + size * 0.08f}, radius, IM_COL32(0, 0, 0, 120), 32);
        draw->AddCircleFilled(centre, radius, IM_COL32(150, 156, 166, 255), 32);
        draw->AddCircleFilled(centre, radius * 0.86f, IM_COL32(236, 238, 242, 255), 32);
        // The house: its roof and its body, in the Remote's blue.
        const float w = radius * 0.95f, h = radius * 0.8f;
        const ImU32 blue = IM_COL32(52, 118, 210, 255);
        const float roof = centre.y - h * 0.08f, ground = centre.y + h * 0.5f;
        draw->AddTriangleFilled({centre.x - w * 0.6f, roof}, {centre.x, centre.y - h * 0.62f},
                                {centre.x + w * 0.6f, roof}, blue);
        draw->AddRectFilled({centre.x - w * 0.38f, roof - 0.5f}, {centre.x + w * 0.38f, ground}, blue);
        draw->AddRectFilled({centre.x - w * 0.1f, centre.y + h * 0.12f}, {centre.x + w * 0.1f, ground},
                            IM_COL32(236, 238, 242, 255));
        return;
    }
    const float round = std::round(size * 0.28f), lip = std::max(2.0f, std::round(size * 0.2f));
    draw->AddRectFilled({x, top + size * 0.08f}, {x + width, bottom + size * 0.08f}, IM_COL32(0, 0, 0, 120), round);
    draw->AddRectFilled({x, top}, {x + width, bottom}, IM_COL32(30, 33, 40, 255), round);
    draw->AddRectFilledMultiColor({x + 2, top + 2}, {x + width - 2, bottom - lip}, IM_COL32(86, 91, 102, 255),
                                  IM_COL32(86, 91, 102, 255), IM_COL32(60, 64, 73, 255), IM_COL32(60, 64, 73, 255));
    draw->AddRect({x, top}, {x + width, bottom}, IM_COL32(150, 156, 168, 200), round, 0, 1.0f);
    const float text = size * 0.82f;
    const ImVec2 measured = font->CalcTextSizeA(text, FLT_MAX, 0, key.c_str());
    draw->AddText(font, text, {std::round(x + (width - measured.x) * 0.5f),
                               std::round(top + (height - lip - measured.y) * 0.5f + 1.0f)},
                  IM_COL32(250, 250, 250, 255), key.c_str());
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

void HostOverlay::SetMenus(HostMenus* menus) { menus_ = menus; }

void HostOverlay::SetPerformance(std::optional<OverlayCorner> corner)
{
    performance_ = corner;
    if (performance_ && !usage_) usage_.emplace();
    else if (!performance_) usage_.reset();
}

void HostOverlay::SetHint(std::optional<Hint> hint) { hint_ = std::move(hint); }

void HostOverlay::BuildFonts(float size, float menuScale)
{
    ImGuiIO& io = ImGui::GetIO();
    for (;;) {
        io.Fonts->Clear();
        io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors;
        // Names and texts may need characters the font lacks: from the system's fonts.
        launcher::PrepareNameGlyphs();
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        const auto add = [&](float pixels) {
            config.SizePixels = pixels;
            if (fontData_.empty()) return io.Fonts->AddFontDefault(&config);
            return io.Fonts->AddFontFromMemoryTTF(fontData_.data(), static_cast<int>(fontData_.size()), pixels,
                                                  &config, kGlyphs);
        };
        const auto addText = [&](float pixels) {
            config.SizePixels = pixels;
            if (fontData_.empty()) return io.Fonts->AddFontDefault(&config);
            ImFont* font = io.Fonts->AddFontFromMemoryTTF(fontData_.data(), static_cast<int>(fontData_.size()),
                                                          pixels, &config, kTextGlyphs);
            if (font) launcher::MergeNameGlyphs(*io.Fonts, pixels);
            return font;
        };
        label_ = add(size);
        messageFont_ = addText(MessageFontSize(size));
        config.FontBuilderFlags |= ImGuiFreeTypeBuilderFlags_Bold;
        number_ = add(size);
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
        // Recharged's menus' (when there are any), at the size they draw
        // them (a lit plate's a little larger than the rest).
        menuFonts_ = {};
        if (menuScale > 0.0f) {
            menuFonts_.title = addText(std::round(ui::kTitleSize * menuScale));
            menuFonts_.plate = addText(std::round(ui::kPlateSize * ui::kLitPlateScale * menuScale));
            menuFonts_.row = addText(std::round(ui::kRowSize * menuScale));
            menuFonts_.small = addText(std::round(ui::kSmallSize * menuScale));
        }
#endif
        if (io.Fonts->Build() || fontData_.empty()) break;
        std::fprintf(stderr, "Host overlay: unreadable font; using the built-in font.\n");
        fontData_.clear();
    }
    // Waits for presentations that still show the previous atlas.
    aurora_imgui_update_overlay_fonts();
    builtSize_ = size;
    builtMenuScale_ = menuScale;
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
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
    const bool menus = menus_ && menus_->IsShown();
#else
    const bool menus = false;
#endif
    const std::optional<Hint> hint = menus ? std::nullopt : hint_;
    const bool showing = performance_ || opacity > 0 || menus || hint;
    if (!showing) {
        if (drawn_) aurora_imgui_clear_overlay();
        drawn_ = false;
        return;
    }
    // The menus are live: every frame while they show.
    if (drawn_ && !menus && !shownMenus_ && values == shownValues_ && count == shownCount_ && message_ == shownMessage_
        && opacity == shownOpacity_ && hint == shownHint_ && surface_width == shownWidth_
        && surface_height == shownHeight_)
        return;
    const auto size = FpsOverlayFontSize(surface_width, surface_height);
    // Rebuilt for another size, or for characters of names they lack.
    // Recharged's menus' fonts: design pixels to the picture's (0: no menus).
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
    const float menuScale = menus_ ? float(surface_height) / ui::kDesignHeight : 0.0f;
#else
    const float menuScale = 0.0f;
#endif
    if (size != builtSize_ || menuScale != builtMenuScale_
        || platform::NameGlyphsVersion() != launcher::NameGlyphsBuilt())
        BuildFonts(size, menuScale);
    aurora_imgui_begin_overlay(surface_width, surface_height);
    // The menus first: the performance numbers and messages stay on top.
#if defined(MSCHARGED_HAS_GAME_OVERLAY)
    if (menus) menus_->Draw(float(surface_width), float(surface_height), menuFonts_);
#endif
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
    const float messageSize = MessageFontSize(size);
    const bool belowPerformance = performance_ && *performance_ == OverlayCorner::TopLeft;
    // The hint: its keys as glyphs (a key of the keyboard, the Wii Remote's
    // HOME), then its text, on a plate in the menus' look.
    float below = 0; // how far a message moves down for it
    const float edge = std::max(2.0f, std::round(messageSize * 0.16f)); // the plates' gold edge
    if (hint) {
        const float gap = std::round(messageSize * 0.45f);
        const auto measure = [&](const std::string& text) {
            return messageFont_->CalcTextSizeA(messageSize, FLT_MAX, 0, text.c_str()).x;
        };
        float width = measure(hint->text) + edge;
        for (const auto& key : hint->keys) width += KeyGlyphWidth(messageFont_, messageSize, key) + gap;
        const auto layout = LayoutOverlayMessage(surface_width, surface_height, messageSize, width,
                                                 belowPerformance ? &*performanceLayout : nullptr);
        const float grow = std::round(messageSize * 0.45f);
        const float top = layout.top, bottom = layout.bottom + 2 * grow, text_y = layout.text_y + grow;
        Plate(draw, {layout.left, top}, {layout.right, bottom}, messageSize, kOpacitySteps);
        float x = layout.text_x + edge;
        const float middle = (top + bottom) * 0.5f;
        for (const auto& key : hint->keys) {
            KeyGlyph(draw, messageFont_, messageSize, x, middle, key);
            x += KeyGlyphWidth(messageFont_, messageSize, key) + gap;
        }
        draw->AddText(messageFont_, messageSize, {x, text_y}, kNumber, hint->text.c_str());
        below = bottom - top + std::round(messageSize * 0.4f);
    }
    if (opacity > 0) {
        const float width = messageFont_->CalcTextSizeA(messageSize, FLT_MAX, 0, message_.c_str()).x + edge;
        auto layout = LayoutOverlayMessage(surface_width, surface_height, messageSize, width,
                                           belowPerformance ? &*performanceLayout : nullptr);
        layout.top += below;
        layout.bottom += below;
        layout.text_y += below;
        Plate(draw, {layout.left, layout.top}, {layout.right, layout.bottom}, messageSize, opacity);
        draw->AddText(messageFont_, messageSize, {layout.text_x + edge, layout.text_y}, WithAlpha(kNumber, opacity),
                      message_.c_str());
    }
    aurora_imgui_end_overlay();
    drawn_ = true;
    shownValues_ = values;
    shownCount_ = count;
    shownMessage_ = message_;
    shownHint_ = hint;
    shownMenus_ = menus;
    shownOpacity_ = opacity;
    shownWidth_ = surface_width;
    shownHeight_ = surface_height;
}

} // namespace mscharged::runtime
