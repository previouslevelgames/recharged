#pragma once
#include "runtime/frontend_navigation.h"
#include <optional>
namespace mscharged
{
// Desktop Back binding. Caller supplies only the actual presented NAV binding
// and runs inside its selected menu input window. Exactly one original action31
// query; a fresh Back wins Confirm and routes through source FEBackButton.
// No binding/hidden geometry/locked input returns no shortcut. This does not
// itself dispatch, pop a scene or manufacture a completed Back handler.
std::optional<FrontendPointerEvent> FrontendDesktopBackEvent(FrontendInput&,
    const std::optional<FrontendNavigationBackBinding>&);
}
