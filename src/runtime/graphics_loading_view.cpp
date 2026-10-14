#include "runtime/graphics_loading_view.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/imgui.h>
#include <imgui.h>
#include <misc/freetype/imgui_freetype.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace mscharged::runtime {

GraphicsLoadingView::GraphicsLoadingView(const std::filesystem::path& font)
{
    std::ifstream input(font, std::ios::binary);
    if (input) fontData_.assign(std::istreambuf_iterator<char>(input), {});
}

void GraphicsLoadingView::BuildFonts(float scale, float density)
{
    auto& io = ImGui::GetIO();
    for (;;) {
        io.Fonts->Clear();
        io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        config.RasterizerDensity = density;
        const auto add = [&](float size) {
            config.SizePixels = std::round(size * scale);
            return fontData_.empty() ? io.Fonts->AddFontDefault(&config)
                : io.Fonts->AddFontFromMemoryTTF(fontData_.data(), int(fontData_.size()),
                    config.SizePixels, &config, io.Fonts->GetGlyphRangesDefault());
        };
        title_ = add(36);
        body_ = add(15);
        caption_ = add(11);
        if (io.Fonts->Build() || fontData_.empty()) break;
        fontData_.clear();
    }
    aurora_imgui_update_overlay_fonts();
    fontScale_ = scale;
    fontDensity_ = density;
}

void GraphicsLoadingView::Draw(std::uint32_t width, std::uint32_t height, const char* label,
                              const AuroraPipelineWarmupStatus& status, bool native)
{
    if (!width || !height) return;
    float density = 1.0f;
    if (!native) {
        // Diagnostic frames use logical window coordinates and apply their
        // framebuffer scale at presentation. Keep the layout in that space;
        // rasterize the atlas at physical density without scaling its metrics.
        const auto size = aurora_get_window_size();
        if (!size.width || !size.height) return;
        density = std::max(float(width) / float(size.width), float(height) / float(size.height));
        width = size.width;
        height = size.height;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!active_ || label_ != label) {
        started_ = last_ = now;
        fraction_ = 0;
        label_ = label;
    }
    active_ = true;
    native_ = native;
    const float scale = std::clamp(std::min(width / 960.0f, height / 540.0f), 0.35f, 4.0f);
    if (scale != fontScale_ || density != fontDensity_) BuildFonts(scale, density);
    const double dt = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    const float target = status.totalPipelines
        ? std::clamp(float(double(status.readyPipelines) / status.totalPipelines), 0.0f, 1.0f) : 0.0f;
    fraction_ += (target - fraction_) * float(1.0 - std::exp(-dt * 9.0));
    const float elapsed = std::chrono::duration<float>(now - started_).count();

    if (native) aurora_imgui_begin_overlay(width, height);
    else if (!aurora_begin_frame()) return;
    auto* draw = ImGui::GetForegroundDrawList();
    const float w = float(width), h = float(height);
    draw->AddRectFilledMultiColor({0, 0}, {w, h}, IM_COL32(13, 22, 29, 255),
        IM_COL32(13, 33, 38, 255), IM_COL32(7, 17, 25, 255), IM_COL32(9, 15, 24, 255));

    // Quiet pitch markings make the screen belong to the game without adding
    // an illustration or any resource loading of its own.
    const float pitchW = 510 * scale, pitchH = 325 * scale;
    const ImVec2 centre{w * 0.76f, h * 0.36f};
    const ImVec2 a{centre.x - pitchW / 2, centre.y - pitchH / 2};
    const ImVec2 b{centre.x + pitchW / 2, centre.y + pitchH / 2};
    const ImU32 line = IM_COL32(151, 214, 211, 14);
    draw->AddRect(a, b, line, 3 * scale, 0, scale);
    draw->AddLine({centre.x, a.y}, {centre.x, b.y}, line, scale);
    draw->AddCircle(centre, 55 * scale, line, 72, scale);
    draw->AddCircleFilled(centre, 2 * scale, line);
    draw->AddRect({a.x, centre.y - 86 * scale}, {a.x + 73 * scale, centre.y + 86 * scale}, line, 0, 0, scale);
    draw->AddRect({b.x - 73 * scale, centre.y - 86 * scale}, {b.x, centre.y + 86 * scale}, line, 0, 0, scale);

    const float left = std::round(std::max(32 * scale, (w - 800 * scale) / 2));
    const float right = w - left;
    const float top = h * 0.56f;
    const auto text = [&](ImFont* font, float x, float y, ImU32 color, const char* value) {
        draw->AddText(font, font->FontSize, {std::round(x), std::round(y)}, color, value);
    };
    const ImU32 accent = IM_COL32(117, 231, 206, 255);
    draw->AddRectFilled({left, top - 26 * scale}, {left + 22 * scale, top - 24 * scale}, accent, scale);
    text(caption_, left + 32 * scale, top - 31 * scale, IM_COL32(161, 188, 195, 255), "GETTING READY");
    text(title_, left, top, IM_COL32(242, 248, 249, 255), label);
    text(body_, left, top + 51 * scale, IM_COL32(151, 172, 182, 255), "Getting everything ready for kick-off.");
    const float barY = top + 98 * scale;
    draw->AddRectFilled({left, barY}, {right, barY + 3 * scale}, IM_COL32(48, 66, 76, 255), 2 * scale);
    if (status.totalPipelines && fraction_ > 0) {
        draw->AddRectFilled({left, barY}, {left + (right - left) * fraction_, barY + 3 * scale}, accent, 2 * scale);
    } else {
        // Discovery has no total yet. Show activity without inventing a percent.
        const float travel = (std::sin(elapsed * 1.5f) + 1) / 2;
        const float segment = (right - left) * 0.13f;
        const float x = left + (right - left - segment) * travel;
        draw->AddRectFilled({x, barY}, {x + segment, barY + 3 * scale}, accent, 2 * scale);
    }
    text(caption_, left, h - 41 * scale, IM_COL32(100, 126, 139, 255), "MARIO STRIKERS CHARGED");
    const float pulse = 0.65f + 0.35f * std::sin(elapsed * 2.6f);
    draw->AddCircleFilled({right - 3 * scale, h - 34 * scale}, 2.5f * scale,
                         IM_COL32(117, 231, 206, int(180 * pulse)));
    if (native) aurora_imgui_end_overlay();
    else aurora_end_frame();
}

bool GraphicsLoadingView::Hide()
{
    if (!active_) return false;
    if (native_) aurora_imgui_clear_overlay();
    active_ = false;
    fontScale_ = 0;
    return true;
}

} // namespace mscharged::runtime
