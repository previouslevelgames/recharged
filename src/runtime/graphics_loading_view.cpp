#include "runtime/graphics_loading_view.h"

#include <aurora/aurora.h>
#include <aurora/imgui.h>
#include <imgui.h>
#include <misc/freetype/imgui_freetype.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace mscharged::runtime {
namespace {
// Nothing is drawn over black before this, so a quick preparation never flashes.
constexpr float kGrace = 0.3f;
constexpr float kFadeIn = 0.35f;
constexpr float kFadeOut = 0.3f;
// After readiness the bar may take this long to complete before fading out.
constexpr float kComplete = 0.25f;
// One way across the track while the total is unknown.
constexpr float kSweep = 1.1f;

ImU32 White(float alpha) { return IM_COL32(255, 255, 255, int(std::lround(255.0f * std::clamp(alpha, 0.0f, 1.0f)))); }

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float EaseInOut(float t)
{
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}
} // namespace

GraphicsLoadingView::GraphicsLoadingView(const std::filesystem::path& font)
{
    std::ifstream input(font, std::ios::binary);
    if (input) fontData_.assign(std::istreambuf_iterator<char>(input), {});
}

void GraphicsLoadingView::BuildFont(float size, float density)
{
    auto& io = ImGui::GetIO();
    for (;;) {
        io.Fonts->Clear();
        io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        config.RasterizerDensity = density;
        config.SizePixels = size;
        font_ = fontData_.empty() ? io.Fonts->AddFontDefault(&config)
            : io.Fonts->AddFontFromMemoryTTF(fontData_.data(), int(fontData_.size()), size, &config,
                                             io.Fonts->GetGlyphRangesDefault());
        if (io.Fonts->Build() || fontData_.empty()) break;
        fontData_.clear();
    }
    aurora_imgui_update_overlay_fonts();
    fontSize_ = size;
    fontDensity_ = density;
}

void GraphicsLoadingView::Draw(const AuroraPipelineWarmupStatus& status) { Frame(false, &status); }

bool GraphicsLoadingView::DrawFinished()
{
    Frame(true, nullptr);
    return opacity_ > 0;
}

void GraphicsLoadingView::Frame(bool finished, const AuroraPipelineWarmupStatus* status)
{
    const auto now = Clock::now();
    if (!started_) {
        started_ = true;
        start_ = last_ = now;
    }
    const float dt = std::min(std::chrono::duration<float>(now - last_).count(), 0.1f);
    last_ = now;
    const float elapsed = std::chrono::duration<float>(now - start_).count();

    if (status) {
        // A resize during the fade-out prepares new targets: resume preparing.
        finishing_ = false;
        // A larger total (another layout) may lower the share; the bar waits.
        determinate_ = status->totalPipelines != 0;
        target_ = determinate_ ? std::clamp(float(double(status->readyPipelines) / double(status->totalPipelines)),
                                            0.0f, 1.0f) : 0.0f;
        if (elapsed >= kGrace) opacity_ = std::min(1.0f, opacity_ + dt / kFadeIn);
    }
    if (finished) {
        if (!finishing_) {
            finishing_ = true;
            finishStart_ = now;
        }
        target_ = 1;
        const float since = std::chrono::duration<float>(now - finishStart_).count();
        if (!determinate_ || progress_ > 0.995f || since >= kComplete)
            opacity_ = std::max(0.0f, opacity_ - dt / kFadeOut);
    }
    const float rate = finishing_ ? 14.0f : 6.0f;
    progress_ = std::max(progress_, progress_ + (target_ - progress_) * (1.0f - std::exp(-dt * rate)));

    // Layout in logical window units; the font rasterizes at physical density.
    const auto size = aurora_get_window_size();
    if (!size.width || !size.height || !size.native_fb_width || !size.native_fb_height) return;
    const float w = float(size.width), h = float(size.height);
    const float density = std::max(1.0f, std::max(float(size.native_fb_width) / w, float(size.native_fb_height) / h));
    const auto snap = [density](float value) { return std::round(value * density) / density; };
    const float scale = std::min(w / 1920.0f, h / 1080.0f);
    const float text = std::clamp(std::round(22.0f * scale), 14.0f, 52.0f);
    if (text != fontSize_ || density != fontDensity_) BuildFont(text, density);

    if (!aurora_begin_frame()) return;
    auto* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled({0, 0}, {w, h}, IM_COL32(0, 0, 0, 255));
    if (opacity_ > 0) {
        const float alpha = Smooth(opacity_);
        const float barWidth = std::min(std::round(text * 20.0f), w - 32.0f);
        const float barHeight = std::max(2.0f, std::round(text * 0.14f));
        const float gap = std::round(text * 0.8f);
        const float line = font_->FontSize;
        const float top = snap((h - (line + gap + barHeight)) / 2.0f);
        const float left = snap((w - barWidth) / 2.0f);
        const float right = left + barWidth;
        const float barTop = snap(top + line + gap);
        const float barBottom = barTop + barHeight;
        const float radius = barHeight / 2.0f;

        draw->AddText(font_, line, {left, top}, White(0.88f * alpha), "Preparing graphics");
        if (determinate_) {
            // 100 only once the renderer is ready; publication can follow the last compile.
            const int percent = finishing_ ? int(std::lround(progress_ * 100.0f))
                                           : std::min(99, int(progress_ * 100.0f));
            char value[8];
            std::snprintf(value, sizeof(value), "%d%%", percent);
            const float valueWidth = font_->CalcTextSizeA(line, w, 0.0f, value).x;
            draw->AddText(font_, line, {snap(right - valueWidth), top}, White(0.45f * alpha), value);
        }
        draw->AddRectFilled({left, barTop}, {right, barBottom}, White(0.12f * alpha), radius);
        if (determinate_) {
            const float fill = snap(barWidth * std::clamp(progress_, 0.0f, 1.0f));
            if (fill > 0)
                draw->AddRectFilled({left, barTop}, {left + fill, barBottom}, White(0.92f * alpha),
                                    std::min(radius, fill / 2.0f));
        } else {
            // The total is not known yet: a short segment glides back and forth.
            const float segment = std::round(barWidth * 0.28f);
            const float phase = std::fmod(elapsed, 2.0f * kSweep) / kSweep;
            const float from = snap(left + (barWidth - segment) * EaseInOut(phase < 1.0f ? phase : 2.0f - phase));
            draw->AddRectFilled({from, barTop}, {from + segment, barBottom}, White(0.92f * alpha), radius);
        }
    }
    aurora_end_frame();
}

} // namespace mscharged::runtime
