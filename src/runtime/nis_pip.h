#pragma once
#include "runtime/nis_playback.h"
#include <optional>

namespace mscharged
{
enum class NisPipMode : unsigned { None = 0, Pip = 1, Swap = 2, Expand = 3 };
struct NisPipRectangle { float x, y, width, height; };
// Selected original overlay state machine over an existing NIS playback owner.
// Advance the playback first, then call Update(step.delta) only if step.active;
// publish the manager pose afterwards. Cameras advance exactly once per frame.
// The playback owner must outlive this object. No Holotron, actor deletion,
// WorldDarkening reset or full NisPlayer lifecycle is implied.
class NisPip
{
    NisPlayback& playback_;
    const std::thread::id thread_ = std::this_thread::get_id();
    NisPipMode mode_ = NisPipMode::Pip;
    float time_ = 0, duration_;
    bool busy_ = false, failed_ = false;
    void Ready() const;
    void CheckThread() const;
public:
    explicit NisPip(NisPlayback& playback, float expansion_duration = 1.f);
    ~NisPip();
    NisPip(const NisPip&) = delete;
    NisPip& operator=(const NisPip&) = delete;
    void SetMode(NisPipMode mode); // Entering Expand resets its original timer.
    void ResetExpansion(); // Only the original expansion Reset; does not change cameras/mode.
    void Update(float delta);
    NisPipMode Mode() const;
    float Time() const;
    bool Failed() const;
    // Logical 640x480 coordinates. None and the transient Swap state draw nothing.
    std::optional<NisPipRectangle> Rectangle() const;
};
}
