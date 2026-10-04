#pragma once
#include "frontend_font_fixture.h"
#include "runtime/frontend_packets.h"
namespace frontend_packet_fixture
{
inline constexpr std::uint32_t ImageHash=0x471fcad1;
inline std::array<float,16> Transform(float x=0,float y=0)
{ return {1,0,0,0,0,1,0,0,0,0,1,0,x,y,0,1}; }
inline std::shared_ptr<mscharged::FrontendSessionFrame> Frame(unsigned variant=0)
{
    using namespace mscharged;
    auto frame=std::make_shared<FrontendSessionFrame>();
    auto font=std::make_shared<resources::FrontendFont>(*resources::ReadFrontendFont(font_fixture::Font(),"fe/fonts/fixture","fixture"));
    auto visuals=std::make_shared<FrontendVisualAssets>(); visuals->text=visuals->heading=font;
    frame->visuals=visuals;
    auto images=std::make_shared<resources::FrontendImageCatalog>();
    auto texture=std::make_shared<resources::Texture>(font->pages[1]); texture->id=ImageHash;
    // RGB5A3: pure blue normally, green for same-hash replacement.
    texture->palette[2]=variant?0x83:0x80; texture->palette[3]=variant?0xe0:0x1f;
    images->textures.emplace(ImageHash,texture); frame->images=images;
    resources::FrontendLayoutText text; text.layout=resources::LayoutFrontendText(font,u"A");
    text.transform=Transform(315,237); text.colour={255,255,255,255};
    resources::FrontendLayoutImage image; image.texture=texture; image.transform=Transform(320,240);
    image.colour={255,255,255,255}; image.blend=1;
    image.vertices={{{-20,20,0,0},{-20,-20,0,1},{20,-20,1,1},{20,20,1,0}}};
    frame->layout.entries={text,image};
    return frame;
}
}
