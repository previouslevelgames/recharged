#pragma once
#include "resources/frontend_scene.h"
#include <algorithm>
#include <bit>
// Canonical diagnostic encoding compared against an independent raw-byte oracle.
// Float bits and ordered references are retained; record visitation order is not.
inline std::uint64_t FrontendSceneFingerprint(const mscharged::resources::FrontendScene& scene)
{
    using namespace mscharged::resources;
    std::uint64_t hash=14695981039346656037ull;
    auto byte=[&](std::uint8_t v){hash=(hash^v)*1099511628211ull;};
    auto word=[&](std::uint32_t v){for(int i=3;i>=0;--i)byte(v>>(8*i));};
    auto number=[&](float v){word(std::bit_cast<std::uint32_t>(v));};
    auto ref=[&](FrontendReference v){word(v.value_or(UINT32_MAX));};
    auto name=[&](const std::string& v){word(v.size());for(auto c:v)byte(c);};
    auto refs=[&](const auto& v){word(v.size());for(auto x:v)word(x);};
    auto attributes=[&](const FrontendAttributes& a){
        for(auto v:a.position)number(v);
        for(auto v:a.rotation)number(v);
        for(auto v:a.scale)number(v);
        for(auto v:a.pivot)number(v);
        word(a.visible);for(auto v:a.colour)word(v);for(auto v:a.uv)number(v);
    };
    auto ordered=[](const auto& values){auto copy=values;std::sort(copy.begin(),copy.end(),[](auto& a,auto& b){return a.offset<b.offset;});return copy;};
    word(scene.id);word(scene.relocation_count);number(scene.presentation_time);ref(scene.active_slide);refs(scene.presentation_slides);
    word(scene.resources.size());for(const auto& r:ordered(scene.resources))
    {word(r.offset);word(r.type);word(r.hash);word(r.file_block);word(r.saved_valid);}
    word(scene.library.size());for(const auto& r:ordered(scene.library))
    {word(r.offset);word(r.type);word(r.hash);name(r.name);attributes(r.attributes);ref(r.resource);ref(r.active_slide);refs(r.slides);for(auto v:r.text_box)number(v);for(auto v:r.text_effect_colour)word(v);}
    word(scene.instances.size());for(const auto& r:ordered(scene.instances))
    {word(r.offset);word(r.type);word(r.hash);word(r.overload_flags);word(r.priority);name(r.name);number(r.start);number(r.duration);word(r.visible);attributes(r.attributes);ref(r.library);ref(r.resource);refs(r.children);word(r.localization_hash);word(r.text_overload_flags);word(r.draw_options);for(auto v:r.text_box)number(v);for(auto v:r.text_effect_colour)word(v);word(r.text.size());for(auto v:r.text)word(v);}
    word(scene.slides.size());for(const auto& r:ordered(scene.slides))
    {word(r.offset);word(r.hash);word(r.play_mode);name(r.name);number(r.start);number(r.duration);number(r.time);word(r.frozen);word(r.animated);refs(r.children);}
    return hash;
}
