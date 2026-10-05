#pragma once
#include "runtime/frontend_input_sdl.h"
#include "runtime/frontend_pointer_host.h"
#include <optional>

namespace mscharged
{
enum class FrontendCursorSource { Mouse, KeyboardGamepad };
struct FrontendCursorMouse
{
    std::uint64_t device=0;
    float x=0,y=0;
    bool connected=false,focused=false,captured=false,down=false;
};
struct FrontendMenuCursorStatus
{
    FrontendCursorSource source=FrontendCursorSource::Mouse;
    std::array<float,2> position{}; // Original centered asset coordinates.
};
// Explicit player-one desktop policy, not Wii DPD emulation. Arrows/D-pad and
// the real gated left stick move at 480 logical units/second (delta<=.1).
// Simultaneous directions normalize to unit speed. Actual mouse movement wins
// simultaneous arbitration; a stationary mouse does not steal virtual control.
// Switching devices, focus/capture, hotplug and viewport/scene changes requires
// neutral input. The existing source pointer route alone queries action30.
class FrontendMenuCursor
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendMenuCursor();
    ~FrontendMenuCursor();
    FrontendMenuCursor(const FrontendMenuCursor&)=delete;
    FrontendMenuCursor& operator=(const FrontendMenuCursor&)=delete;
    // Call only for the actual renderer-acknowledged scene/token and viewport.
    void Acknowledge(std::uint64_t scene,FrontendSession::Handle,FrontendPointerViewport);
    FrontendPointerDesktopSample Sample(const FrontendInputSnapshot&,const FrontendCursorMouse&);
    // Samples real absolute mouse state without another event pump/FE update.
    // Resize/suspension returns nullopt; caller uses its existing source Leave
    // route, then acknowledges a new viewport before resuming this cursor.
    std::optional<FrontendPointerDesktopSample> Poll(const FrontendInputSnapshot&,SDL_Window*,bool mouse_capture=false);
    FrontendMenuCursorStatus Status() const;
};
}
