#pragma once

#include "bootstrap/graphics_card.h"

#include <string>
#include <vector>

namespace mscharged::platform
{
// A graphics card the PC offers the game.
struct GraphicsCard
{
    GraphicsCardId id;
    std::string name;
};

// The PC's graphics cards as Windows (DXGI) or, on Linux, the Vulkan driver
// lists them: no software renderers, each PCI ID once. A card switched off
// (in Device Manager or by a laptop's GPU mode) is not there. Empty on macOS
// and when nothing can be listed. Takes a moment: call it off the UI thread.
std::vector<GraphicsCard> ListGraphicsCards();

// "Graphics cards: NAME (10de:1f99), ..." for the log.
std::string DescribeGraphicsCards(const std::vector<GraphicsCard>& cards);
}
