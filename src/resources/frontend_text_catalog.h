#pragma once
#include "resources/frontend_fonts.h"
#include "resources/frontend_scene.h"
#include <map>

namespace mscharged::resources
{
struct FrontendTextEntry
{
    std::uint32_t instance;
    std::string name;
    std::u16string text;
    FontLayout layout;
};
struct FrontendTextCatalog
{
    std::vector<FrontendTextEntry> entries;
    std::map<std::string, unsigned> unavailable;
};
// Inspect stored text components across every slide. This does not select an
// active timeline or implement the missing original textbox/layout renderer.
// Retained font owners let a catalog outlive the source scene and localization.
FrontendTextCatalog InspectFrontendText(const FrontendScene&, const Localization&,
    std::span<const std::shared_ptr<const FrontendFont>> fonts);
}
