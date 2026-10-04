#include "resources/frontend_text_catalog.h"
#include <algorithm>

namespace mscharged::resources
{
FrontendTextCatalog InspectFrontendText(const FrontendScene& scene, const Localization& localization,
    std::span<const std::shared_ptr<const FrontendFont>> fonts)
{
    Require(scene.instances.size() <= 16384, "Frontend text instance limit exceeded");
    std::map<std::uint32_t, const FrontendLibraryObject*> library;
    std::map<std::uint32_t, const FrontendResource*> resources;
    for (const auto& value : scene.library)
        Require(library.emplace(value.offset, &value).second, "Duplicate frontend library ID");
    for (const auto& value : scene.resources)
        Require(resources.emplace(value.offset, &value).second, "Duplicate frontend resource ID");
    FrontendTextCatalog result;
    std::size_t glyphs = 0;
    for (const auto& instance : scene.instances)
    {
        if (instance.type != 3) continue;
        Require(instance.library && library.contains(*instance.library), "Text instance has no library object");
        const auto& object = *library.at(*instance.library);
        Require(object.type == 2, "Text instance has a non-text library object");
        if (!object.resource)
        { ++result.unavailable["font assigned by a scene handler"]; continue; }
        Require(resources.contains(*object.resource), "Text font resource is absent");
        const auto& resource = *resources.at(*object.resource);
        Require(resource.type == 1, "Text font reference has the wrong resource type");
        auto selected = std::find_if(fonts.begin(), fonts.end(), [&](const auto& font) {
            return font && font->alias == resource.hash;
        });
        if (selected == fonts.end())
        { ++result.unavailable["font alias outside the selected text/heading pair"]; continue; }
        std::u16string text;
        if (instance.text_overload_flags & 8)
            text = localization.Get(instance.localization_hash);
        else if (!instance.text.empty()) text = instance.text;
        else
        { ++result.unavailable["text assigned by a scene handler"]; continue; }
        if (text.empty())
        { ++result.unavailable["empty text"]; continue; }
        // Original escape expansion, controller icons and textbox rules need
        // separate services. Never silently strip or replace these sequences.
        if (text.size() > 4096 || std::any_of(text.begin(), text.end(), [](char16_t c) {
            return c == '{' || c == '}' || c < 32 || c == 127 || (c >= 0xd800 && c <= 0xdfff);
        }))
        { ++result.unavailable["formatted, multiline or extended text"]; continue; }
        auto layout = LayoutFrontendText(*selected, text);
        glyphs += layout.quads.size();
        Require(glyphs <= 262144, "Frontend text catalog glyph budget exceeded");
        result.entries.push_back({instance.offset, instance.name, std::move(text), std::move(layout)});
    }
    return result;
}
}
