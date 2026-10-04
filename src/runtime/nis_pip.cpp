#include "runtime/nis_pip.h"
#include "Game/Render/NisPipSteps.h"
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

namespace mscharged
{
NisPip::NisPip(NisPlayback& playback, float expansion_duration)
    : playback_(playback), duration_(expansion_duration)
{
    if (!std::isfinite(duration_) || duration_ <= 0)
        throw std::invalid_argument("NIS PIP expansion duration must be finite and positive");
    Ready();
}
NisPip::~NisPip()
{
    if (thread_ != std::this_thread::get_id() || busy_) std::terminate();
}
void NisPip::CheckThread() const
{
    if (thread_ != std::this_thread::get_id()) throw std::logic_error("NIS PIP requires its owner thread");
}
void NisPip::Ready() const
{
    CheckThread();
    if (busy_ || failed_) throw std::logic_error("NIS PIP is busy or failed");
    playback_.LastStep(); // Also validates the retained camera session and callback guard.
}
void NisPip::SetMode(NisPipMode mode)
{
    Ready();
    if (static_cast<unsigned>(mode) > 3)
        throw std::invalid_argument("Unsupported NIS overlay mode (Holotron is not selected)");
    if (mode == NisPipMode::Expand && mode_ != mode) time_ = 0;
    mode_ = mode;
}
void NisPip::ResetExpansion() { Ready(); time_ = 0; }
void NisPip::Update(float delta)
{
    Ready(); CheckCameraDelta(delta);
    if (mode_ == NisPipMode::Expand && double(time_) + delta > std::numeric_limits<float>::max())
        throw std::overflow_error("NIS PIP elapsed time exceeds finite original arithmetic");
    struct Operation
    {
        bool& busy; bool& failed; int exceptions = std::uncaught_exceptions();
        Operation(bool& b, bool& f) : busy(b), failed(f) { busy = true; }
        ~Operation() { failed |= std::uncaught_exceptions() > exceptions; busy = false; }
    } operation(busy_, failed_);
    struct Player
    {
        NisPlayback& playback;
        void SwapCameras() { playback.Swap(); }
    } player{playback_};
    if (mode_ == NisPipMode::Swap)
        mode_ = static_cast<NisPipMode>(AdvanceNisPipSwap(&player));
    else if (mode_ == NisPipMode::Expand)
        mode_ = static_cast<NisPipMode>(AdvanceNisPipExpansion(&player, time_, duration_, delta));
}
NisPipMode NisPip::Mode() const { Ready(); return mode_; }
float NisPip::Time() const { Ready(); return time_; }
bool NisPip::Failed() const { CheckThread(); return failed_; }
std::optional<NisPipRectangle> NisPip::Rectangle() const
{
    Ready();
    if (mode_ == NisPipMode::None || mode_ == NisPipMode::Swap) return std::nullopt;
    const auto rectangle = mode_ == NisPipMode::Pip ? NisPipFixedRectangle()
        : NisPipExpandedRectangle(time_, duration_);
    return NisPipRectangle{rectangle.x, rectangle.y, rectangle.width, rectangle.height};
}
}
