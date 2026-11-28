#pragma once

#include "overlay/game_options.h"
#include "overlay/game_settings.h"
#include "overlay/online_panel.h"
#include "overlay/ranked_tab.h"

// What the host hands the menus' pages each frame (src/ui/rui_pages.h's
// PageModel holds it): the online pages' data, RANKED's, Recharged's gameplay
// options and the settings.
namespace mscharged::overlay
{
struct GameOverlayModel
{
    OnlinePanelModel online;
    RankedModel ranked;
    GameOptionsModel options;
    GameSettingsModel settings;
};

} // namespace mscharged::overlay
