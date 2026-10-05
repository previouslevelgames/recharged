#include "runtime/frontend_menu_back.h"
#include <cmath>
#include <stdexcept>
namespace mscharged
{
std::optional<FrontendPointerEvent> FrontendDesktopBackEvent(FrontendInput& input,
    const std::optional<FrontendNavigationBackBinding>& binding)
{
    const bool pressed=input.Button(FrontendAction::Back,FrontendButtonQuery::Pressed,0);
    if(!pressed||!binding||!binding->visible)return {};
    if(!binding->frame||!binding->component)throw std::logic_error("Desktop Back requires actual retained NAV identity");
    const auto& b=binding->bounds;
    const float x=(b.min_x+b.max_x)/2-b.pivot[0],y=(b.min_y+b.max_y)/2-b.pivot[1];
    FrontendPointerEvent event{0,{b.pivot[0]+std::cos(b.rotation)*x-std::sin(b.rotation)*y,
        b.pivot[1]+std::sin(b.rotation)*x+std::cos(b.rotation)*y},true};
    if(!FrontendPointerContains(b,event.position))throw std::logic_error("Desktop Back center is outside original bounds");
    return event;
}
}
