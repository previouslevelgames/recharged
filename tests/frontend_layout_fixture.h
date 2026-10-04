#pragma once
#include "resources/frontend_scene.h"

namespace frontend_layout_fixture
{
inline mscharged::resources::FrontendAttributes Attributes()
{
    mscharged::resources::FrontendAttributes value;
    value.scale={1,1,1};value.colour={255,255,255,255};value.visible=true;return value;
}
inline mscharged::resources::FrontendScene Scene(std::uint32_t font_alias)
{
    using namespace mscharged::resources;
    FrontendScene scene{};scene.active_slide=100;scene.presentation_slides={100};
    scene.resources.push_back({10,1,font_alias,0,false});
    FrontendLibraryObject layer{};layer.offset=20;layer.type=0;layer.attributes=Attributes();
    FrontendLibraryObject text{};text.offset=30;text.type=2;text.attributes=Attributes();text.resource=10;
    scene.library={layer,text};
    FrontendSlide slide{};slide.offset=100;slide.name="Static";slide.duration=100;slide.children={200};scene.slides={slide};
    FrontendInstance parent{};parent.offset=200;parent.type=1;parent.library=20;parent.name="Parent";
    parent.duration=100;parent.visible=true;parent.attributes=Attributes();parent.children={300,400,500};
    scene.instances.push_back(parent);
    for(unsigned i=0;i<3;++i)
    {
        FrontendInstance label{};label.offset=300+i*100;label.type=3;label.library=30;
        label.name="Text"+std::to_string(i);label.duration=100;label.visible=true;label.attributes=Attributes();
        label.attributes.position={float(-160+160*int(i)),float(120-120*int(i)),0};label.overload_flags=1|16;
        label.priority=std::uint16_t(768-i*256);label.text=u"AB";label.text_box={128,64};
        scene.instances.push_back(label);
    }
    return scene;
}
}
