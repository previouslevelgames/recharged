#pragma once

#include <aurora/pipeline_cache.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ImFont;

namespace mscharged::runtime {

// Presentation only. Native mode publishes an overlay through the existing VI
// presenter; it never starts a diagnostic GX frame or advances the game.
class GraphicsLoadingView {
public:
    explicit GraphicsLoadingView(const std::filesystem::path& font);
    void Draw(std::uint32_t width, std::uint32_t height, const char* label,
              const AuroraPipelineWarmupStatus& status, bool native);
    // Returns true when an overlay was retired. Other overlay owners must then
    // rebuild their fonts, because ImGui has one shared atlas.
    bool Hide();

private:
    void BuildFonts(float scale, float density);
    std::vector<unsigned char> fontData_;
    ImFont* title_ = nullptr;
    ImFont* body_ = nullptr;
    ImFont* caption_ = nullptr;
    float fontScale_ = 0;
    float fontDensity_ = 0;
    float fraction_ = 0;
    bool active_ = false;
    bool native_ = false;
    std::string label_;
    std::chrono::steady_clock::time_point last_{};
    std::chrono::steady_clock::time_point started_{};
};

} // namespace mscharged::runtime
