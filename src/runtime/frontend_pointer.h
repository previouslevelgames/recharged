#pragma once
#include "runtime/frontend_input.h"
#include "runtime/frontend_session.h"
#include <array>
#include <functional>

namespace mscharged
{
struct FrontendPointerEvent
{
    unsigned index = 0;
    std::array<float,2> position{}; // Original FE asset coordinates, X right / Y up.
    bool pressed = false, released = false, unidentified = false;
};
enum class FrontendPointerCallback { Enter, Update, Inside, Press, Unidentified, Leave, Release };
struct FrontendPointerBounds
{
    float min_x = 0, max_x = 0, min_y = 0, max_y = 0, rotation = 0;
    std::array<float,2> pivot{};
};
struct FrontendPointerBinding
{
    std::uint32_t instance = 0;
    bool use_rotation = false;
    float offset_x = 0, offset_y = 0, scale_x = 1, scale_y = 1;
    unsigned screen_height = 480; // Original list extent sentinel uses height/2 and fixed 854/2.
};
// Checked original bounds equations over an immutable native scene snapshot.
// Text requires a published original ProcessString layout for that instance;
// missing/unsupported text remains an explicit error, never a guessed rectangle.
FrontendPointerBounds MeasureFrontendPointerBounds(const FrontendSession::Handle&, const FrontendPointerBinding&);
bool FrontendPointerContains(const FrontendPointerBounds&, std::array<float,2>);

// One native listener, not the original global FEPointerManager or a Wii DPD
// producer. Deliver is explicit host event routing through the original listener
// algorithm. The input owner must outlive this object; create/use/destroy on its
// thread. Frame/font/texture handles survive session replacement and Pop.
class FrontendPointerRegion
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using Frame = FrontendSession::Handle;
    using Callback = std::function<void(FrontendPointerCallback, unsigned, const Frame&)>;
    FrontendPointerRegion(FrontendInput&, Frame, FrontendPointerBinding, Callback = {});
    ~FrontendPointerRegion();
    FrontendPointerRegion(const FrontendPointerRegion&) = delete;
    FrontendPointerRegion& operator=(const FrontendPointerRegion&) = delete;
    // Rebinding preserves previous pointer history, as original SetInstanceBounds
    // does. Failure retains the current frame and bounds. Disable resets history.
    void Rebind(Frame, FrontendPointerBinding);
    void SetBounds(float min_x, float max_x, float max_y, float min_y); // Keeps original rotation/pivot.
    void Enable();
    void Disable();
    void IgnoreInputLock(bool);
    bool Enabled() const;
    bool Contains(std::array<float,2>) const;
    FrontendPointerBounds Bounds() const;
    Frame Current() const;
    void Deliver(const FrontendPointerEvent&);
    void Deliver(const Frame& expected, const FrontendPointerEvent&); // Rejects stale/foreign routing.
    // Callback exceptions leave that pointer's previous event uncommitted; any
    // callback side effects already performed are not rolled back. Callback
    // queries are valid, recursive delivery/mutation/release are rejected.
    // Destroying the listener inside its callback is forbidden.
    void Release();
};
}
