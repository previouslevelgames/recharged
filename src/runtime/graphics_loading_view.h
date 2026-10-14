#pragma once

#include <aurora/pipeline_cache.h>
#include <chrono>
#include <filesystem>
#include <vector>

struct ImFont;

namespace mscharged::runtime {

// The graphics preparation screen shown before the game starts: a label and
// percentage over a thin progress bar, centred on black. It fades in only when
// preparation takes longer than a moment and fades out to the black the game
// starts from. Presentation only: Aurora frames before original GX exists.
class GraphicsLoadingView {
public:
    explicit GraphicsLoadingView(const std::filesystem::path& font);
    // One frame while the renderer prepares pipelines.
    void Draw(const AuroraPipelineWarmupStatus& status);
    // One frame after preparation: completes the bar, then fades out. Returns
    // false once a black frame has been presented and nothing remains shown.
    bool DrawFinished();

private:
    using Clock = std::chrono::steady_clock;
    void Frame(bool finished, const AuroraPipelineWarmupStatus* status);
    void BuildFont(float size, float density);

    std::vector<unsigned char> fontData_;
    ImFont* font_ = nullptr;
    float fontSize_ = 0;
    float fontDensity_ = 0;
    bool started_ = false;
    Clock::time_point start_{};
    Clock::time_point last_{};
    Clock::time_point finishStart_{};
    bool finishing_ = false;
    bool determinate_ = false;
    float target_ = 0;    // ready share of the known total
    float progress_ = 0;  // displayed share, eased and never decreasing
    float opacity_ = 0;   // of everything drawn over the black background
};

} // namespace mscharged::runtime
