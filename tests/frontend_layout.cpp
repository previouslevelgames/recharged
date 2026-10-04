#include "resources/frontend_layout.h"
#include "frontend_layout_fixture.h"
#include "frontend_font_fixture.h"
#include "frontend_image_fixture.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool good,const char* reason){++checks;if(!good)throw std::runtime_error(reason);}
void Near(float a,float b,float tolerance=.0001f){Check(std::isfinite(a)&&std::abs(a-b)<tolerance,"Frontend transform/textbox oracle differs");}
template<class F>void Reject(F action){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error("Invalid frontend frame accepted");}
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot read "+path.string());return {std::istreambuf_iterator<char>(file),{}};}
std::shared_ptr<const FrontendFont> Font(){return ReadFrontendFont(font_fixture::Font(),"fe/fonts/fixture","fixture");}
struct Fixture
{
    std::shared_ptr<const FrontendFont> font=Font();
    std::array<std::shared_ptr<const FrontendFont>,1> fonts{font};
    Localization localization;
    FrontendScene scene=frontend_layout_fixture::Scene(font->alias);
    FrontendLayoutFrame Build(){return BuildFrontendLayout(scene,localization,fonts);}
    FrontendInstance& Parent(){return scene.instances[0];}
    FrontendInstance& Text(unsigned i=0){return scene.instances[i+1];}
};
const FrontendLayoutText& Entry(const FrontendLayoutFrame& frame,unsigned id=300)
{const auto it=std::find_if(frame.entries.begin(),frame.entries.end(),[&](auto& entry){return std::holds_alternative<FrontendLayoutText>(entry)&&std::get<FrontendLayoutText>(entry).instance==id;});Check(it!=frame.entries.end(),"Expected authored text is absent");return std::get<FrontendLayoutText>(*it);}
void Baseline()
{
    Fixture f;auto result=f.Build();Check(result.TextCount()==3&&result.unavailable.empty()&&result.hidden==0,"Static frame did not select its three text components");
    Check(std::get<FrontendLayoutText>(result.entries[0]).instance==500&&std::get<FrontendLayoutText>(result.entries[1]).instance==400&&std::get<FrontendLayoutText>(result.entries[2]).instance==300,"Original Anark reverse submission order changed");
    for(unsigned i=0;i<3;++i)
    {
        const auto& entry=Entry(result,300+100*i);Near(entry.transform[12],160+160*i);Near(entry.transform[13],120+120*i);
        Near(entry.transform[0],1);Near(entry.transform[5],1);Check(entry.layout.quads.size()==2,"Plain text glyph count differs");
        Near(entry.layout.quads[0].left,-1);Near(entry.layout.quads[0].top,-2);Near(entry.layout.width,19);
    }
    f.scene.instances.clear();f.font.reset();f.fonts={};Check(Entry(result).layout.font&&Entry(result).text==u"AB","Frame lost retained font/text after source release");
}
void Inheritance()
{
    Fixture f;f.Parent().overload_flags=1|4|8|16;
    f.Parent().attributes.position={11,17,0};f.Parent().attributes.scale={2,3,1};f.Parent().attributes.pivot={4,5,0};
    f.Parent().attributes.colour={128,200,100,128};f.Text().attributes.colour={200,100,128,64};
    f.Text().attributes.position={7,9,0};auto result=f.Build();const auto& entry=Entry(result);
    // Column-vector analytic (child x+7,y+9), then parent(x-4)*2+11,(y-5)*3+17.
    Near(entry.transform[0],2);Near(entry.transform[5],3);Near(entry.transform[12],337);Near(entry.transform[13],211);
    const std::array<unsigned,4> parent{128,200,100,128},child{200,100,128,64};
    for(unsigned i=0;i<4;++i)Check(entry.colour[i]==unsigned(float(float(child[i]*float(parent[i]/255.f))/255.f)*255.f),"Inherited original colour arithmetic differs");
    f.Text().overload_flags=0;f.scene.library[1].attributes.position={1,2,0};
    result=f.Build();Near(Entry(result).transform[12],325);Near(Entry(result).transform[13],232);
    f.Parent().overload_flags=2;f.Parent().attributes.rotation={0,0,.73f};f.Text().overload_flags=1;f.Text().attributes.position={40,20,0};
    result=f.Build();const auto& rotated=Entry(result);
    // Independent analytic geometry allows fixed16 angle/table approximation.
    const double x=40*std::cos(.73)-20*std::sin(.73),y=40*std::sin(.73)+20*std::cos(.73);
    Near(rotated.transform[12],float(320+x),.012f);Near(rotated.transform[13],float(240-y),.012f);
    Near(rotated.transform[0],std::cos(.73),.0003f);Near(rotated.transform[1],-std::sin(.73),.0003f);
}
void TextBoxes()
{
    Fixture f;f.Text().text_box={128,64};
    for(unsigned horizontal=0;horizontal<3;++horizontal)for(unsigned vertical:{0u,0x10u,0x20u})for(unsigned leading:{0u,0x200u})
    {
        f.Text().draw_options=horizontal|vertical|leading;auto result=f.Build();const auto& entry=Entry(result);
        const float dx=horizontal==0?0:horizontal==1?-10:-19;
        const float dy=(vertical==0?0:vertical==0x10?-6:-12)+(leading?1:0);
        Near(entry.layout.quads[0].left,-1+dx);Near(entry.layout.quads[0].top,-2+dy);
    }
    f.Text().draw_options=0;f.Text().text=u"AB AB";f.Text().text_box={30,64};auto result=f.Build();const auto& wrapped=Entry(result);
    Check(wrapped.layout.quads.size()==5,"Textbox wrapping lost glyphs");Near(wrapped.layout.quads[3].left,-1);Near(wrapped.layout.quads[3].top,10);Near(wrapped.layout.height,24);
    f.Text().draw_options=0x1000;result=f.Build();Near(Entry(result).layout.quads[3].top,-2);Near(Entry(result).layout.height,12);
    f.Text().text=u"AAAAAAAAAAAAAAAAA";f.Text().draw_options=0x400;f.Text().text_box={9,64};Reject([&]{f.Build();});
    f.Text().text=u"A";f.Text().text_box={8,64};Reject([&]{f.Build();});
    f.Text().text_box={100,64};f.Text().text=u"A\u00e9";f.Text().localization_hash=1;f.Text().text_overload_flags=8;f.localization.strings[1]=u"BA";
    result=f.Build();Check(Entry(result).text==u"BA","Original localization flag lost precedence");
    f.Text().text_overload_flags=0;result=f.Build();Check(Entry(result).layout.quads.size()==2,"Extended font character mapping changed");
}
void ParagraphsAndFallback()
{
    Fixture f;f.Text().text_box={128,128};f.Text().draw_options=0;f.Text().text=u"A{p}B";
    const FrontendLayoutOptions options{true,true};
    const auto build=[&]{return BuildFrontendLayout(f.scene,f.localization,f.fonts,{},options);};
    Check(f.Build().TextCount()==2,"Strict layout accepted paragraph formatting");
    auto frame=build();const auto& entry=Entry(frame);Check(entry.layout.quads.size()==2,"Paragraph generated extra glyphs");
    Near(entry.layout.quads[0].left,-1);Near(entry.layout.quads[0].top,-2);
    Near(entry.layout.quads[1].left,0);Near(entry.layout.quads[1].top,10);Near(entry.layout.height,24);
    for(const auto text:{u"{p}A",u"A{p}",u"A{p}{p}B"})
    {f.Text().text=text;frame=build();const auto& e=Entry(frame);Near(e.layout.height,text==std::u16string_view(u"A{p}{p}B")?36:24);}
    f.Text().text=u"A{p}{p}B";frame=build();Near(Entry(frame).layout.quads[1].top,22);
    for(const auto text:{u"A{nbs}B",u"A{p",u"A{clr:FFFFFF}B",u"A}B"})
    {f.Text().text=text;Check(build().TextCount()==2,"Unqualified formatting became plain text");}
    f.Text().text=u"AB";f.scene.resources[0].hash=0xbadcafe;
    Check(f.Build().TextCount()==0,"Strict layout invented missing font binding");
    frame=build();Check(frame.TextCount()==3&&frame.font_fallbacks.at(0xbadcafe)==f.font->alias,"Original fallback was not explicit in layout result");
    Check(Entry(frame).layout.font==f.font,"Fallback lost retained font identity");
    f.Text().text=u"A";for(unsigned i=0;i<17;++i)f.Text().text+=u"{p}A";Reject(build);
}
void Selection()
{
    Fixture f;f.scene.slides[0].time=2;f.Text().start=2;f.Text().duration=0;
    Check(f.Build().TextCount()==3,"Exact time endpoint was rejected");
    f.scene.slides[0].time=2.0002f;Check(f.Build().TextCount()==2,"Out-of-time instance was rendered");
    f.scene.slides[0].time=2.00005f;Check(f.Build().TextCount()==3,"Original near-time tolerance was discarded");
    f.Parent().visible=false;auto result=f.Build();Check(result.entries.empty()&&result.hidden==1,"Invisible parent leaked descendants");
    f.Parent().visible=true;f.scene.library[0].attributes.visible=false;Check(f.Build().entries.empty(),"Invisible library leaked descendants");
    f.scene.library[0].attributes.visible=true;f.Parent().attributes.visible=false;Check(f.Build().TextCount()==3,"Unused overloaded visibility field changed original semantics");
    f.scene.slides[0].animated=true;result=f.Build();Check(result.entries.empty()&&result.unavailable.at("animated slide branch")==1,"Animation payload was silently treated as a static frame");
    auto alternative=f.scene.slides[0];alternative.offset=101;alternative.animated=false;alternative.name="Alternative";
    f.scene.slides.push_back(alternative);f.scene.presentation_slides.push_back(101);
    Check(BuildFrontendLayout(f.scene,f.localization,f.fonts,101).TextCount()==3,"Explicit authored presentation slide was not selected");
    Reject([&]{BuildFrontendLayout(f.scene,f.localization,f.fonts,999);});
    f.scene.active_slide.reset();Check(f.Build().entries.empty(),"Absent active slide invented a default");
}
void Components()
{
    Fixture f;auto& parent=f.Parent();parent.type=4;f.scene.library[0].type=3;
    f.scene.library[0].slides={101};f.scene.library[0].active_slide=101;
    auto nested=f.scene.slides[0];nested.offset=101;nested.children=parent.children;parent.children.clear();f.scene.slides.push_back(nested);
    Check(f.Build().TextCount()==3,"Stored component active slide was not followed");
    f.scene.library[0].active_slide.reset();Check(f.Build().entries.empty(),"Component without active slide invented a choice");
    f.scene.library[0].active_slide=101;f.scene.slides[1].children={200};Reject([&]{f.Build();});
}
void UnsupportedAndErrors()
{
    Fixture f;
    for(auto text:{u"A{b}",u"A\nB",u"\U0001f600",u"AX"})
    {f.Text().text=text;auto result=f.Build();Check(result.TextCount()==2&&result.unavailable.size()==1,"Unsupported text was not explicitly omitted");}
    f.Text().text=u"AB";f.Text().text_scissor=true;Check(f.Build().TextCount()==2,"Scissored text lost clipping without rejection");f.Text().text_scissor=false;
    for(unsigned options:{4u,8u,0x100u,0x2000u,0x30u}){f.Text().draw_options=options;Check(f.Build().TextCount()==2,"Unqualified draw options were accepted");}
    f.Text().draw_options=0;f.Parent().overload_flags=2;f.Parent().attributes.rotation[0]=.1f;
    Check(f.Build().entries.empty(),"Nonplanar ancestor branch was rendered");
    f.Parent().attributes.rotation[0]=NAN;Reject([&]{f.Build();});
    f.Parent().attributes.rotation[0]=0;f.Text().duration=-1;Reject([&]{f.Build();});f.Text().duration=100;
    f.Parent().children.push_back(200);Reject([&]{f.Build();});f.Parent().children.pop_back();
    f.Parent().children.push_back(999);Reject([&]{f.Build();});f.Parent().children.pop_back();
    f.Text().library=999;Reject([&]{f.Build();});f.Text().library=30;
    f.scene.instances.push_back(f.Text());Reject([&]{f.Build();});f.scene.instances.pop_back();
    f.Text().text_box={32768,64};Reject([&]{f.Build();});
}
void Images()
{
    Fixture f;constexpr unsigned hash=0x3456789;frontend_image_fixture::AddImage(f.scene,hash);
    FrontendImageCatalog images;auto texture=frontend_image_fixture::Texture(hash);images.textures[hash]=texture;
    const auto build=[&]{return BuildFrontendLayout(f.scene,f.localization,f.fonts,{},images);};
    auto result=build();Check(result.TextCount()==3&&result.ImageCount()==1&&result.unavailable.empty(),"Image was not mixed with text");
    Check(std::get<FrontendLayoutImage>(result.entries[0]).instance==600,"Mixed Anark order grouped images/text incorrectly");
    auto& image=f.scene.instances.back();auto& object=f.scene.library.back();
    object.attributes.uv={.1f,.2f,.6f,.4f};image.attributes.uv={.3f,.4f,.2f,.1f};
    for(unsigned mask=0;mask<16;++mask)
    {
        image.overload_flags=1|16|(mask<<6);result=build();const auto& entry=std::get<FrontendLayoutImage>(result.entries[0]);
        std::array<float,4> uv;for(unsigned i=0;i<4;++i)uv[i]=(mask&(1u<<i))?image.attributes.uv[i]:object.attributes.uv[i];
        const float left=uv[0]+1.f/32,right=uv[0]+uv[2]-1.f/32;
        const float top=1-uv[1]-1.f/32,bottom=1-(uv[1]+uv[3])+1.f/32;
        const std::array<float,4> x{-50,-50,50,50},y{50,-50,-50,50},u{left,left,right,right},v{bottom,top,top,bottom};
        for(unsigned i=0;i<4;++i){Near(entry.vertices[i].x,x[i]);Near(entry.vertices[i].y,y[i]);Near(entry.vertices[i].u,u[i]);Near(entry.vertices[i].v,v[i]);}
        Near(entry.transform[12],320);Near(entry.transform[13],240);Near(entry.transform[0],1);Near(entry.transform[5],-1);
    }
    image.overload_flags=1|16;image.attributes.position={7,9,0};image.attributes.colour={200,100,128,64};
    f.Parent().overload_flags=1|4|8|16;f.Parent().attributes.position={11,17,0};f.Parent().attributes.scale={2,3,1};
    f.Parent().attributes.pivot={4,5,0};f.Parent().attributes.colour={128,200,100,128};result=build();
    const auto& inherited=std::get<FrontendLayoutImage>(result.entries[0]);Near(inherited.transform[0],2);Near(inherited.transform[5],-3);
    Near(inherited.transform[12],337);Near(inherited.transform[13],211);Check(inherited.colour==std::array<std::uint8_t,4>{100,78,50,32},"Image inherited colour changed");
    // A text command before AND after the image proves global reversal.
    f.Parent().children={300,600,400,500};result=build();
    Check(std::holds_alternative<FrontendLayoutText>(result.entries[0])&&std::holds_alternative<FrontendLayoutText>(result.entries[1])
        &&std::holds_alternative<FrontendLayoutImage>(result.entries[2])&&std::holds_alternative<FrontendLayoutText>(result.entries[3]),"Mixed command sequence changed");
    for(unsigned blend=0;blend<8;++blend){image.image_blend=blend;result=build();Check(std::get<FrontendLayoutImage>(result.entries[2]).blend==blend,"Original image blend changed");}
    image.image_blend=8;Reject(build);image.image_blend=1;
    image.overload_flags|=0x40;image.attributes.uv[0]=32;Reject(build);image.attributes.uv[0]=NAN;Reject(build);image.attributes.uv[0]=-.125f;
    result=build();Check(result.ImageCount()==1,"Original repeated/flipped UV domain was constrained to unit range");
    images.unavailable[hash]="unavailable source";Reject(build);images.textures.clear();result=build();
    Check(result.ImageCount()==0&&result.unavailable.at("unavailable source")==1,"Unavailable resource was silently rendered");images.unavailable.clear();
    result=build();Check(result.ImageCount()==0&&result.unavailable.at("image texture outside the supplied set")==1,"Missing image resource became a placeholder");
    images.textures[hash]=texture;image.resource.reset();Check(build().ImageCount()==0,"Handler-assigned image invented a resource");image.resource=601;
    f.scene.resources.back().hash=FrontendNameHash("movie");images.textures[f.scene.resources.back().hash]=texture;
    result=build();Check(result.ImageCount()==0&&result.unavailable.at("dynamic movie or grab image")==1,"Dynamic movie image accepted an ordinary texture");
    f.scene.resources.back().hash=hash;images.textures.erase(FrontendNameHash("movie"));
    texture->width=0;Reject(build);texture->width=16;texture->pixels.pop_back();Reject(build);texture=frontend_image_fixture::Texture(hash);images.textures[hash]=texture;
    result=build();auto retained=std::get<FrontendLayoutImage>(result.entries[2]).texture;
    images.textures.clear();texture.reset();f.scene={};Check(retained&&retained->pixels.size()==1024,"Image storage died with source catalog");
    for(unsigned format:{1u,2u,3u,8u}){auto t=frontend_image_fixture::Texture(hash,format);ValidateFrontendImageTexture(*t);t->levels=12;Reject([&]{ValidateFrontendImageTexture(*t);});}
}
void Owned(const std::filesystem::path& root)
{
    const std::array fonts{
        ReadFrontendFont(Read(root/"fonts/eurfonttext18.res"),"fe/fonts/eurfonttext18","fot-rodinprob18"),
        ReadFrontendFont(Read(root/"fonts/eurfontheading36.res"),"fe/fonts/eurfontheading36","Scratchy36")};
    const auto loc=ReadLocalization(Read(root/"english.loc"),0x7a947b29);
    unsigned files=0,frames=0,entries=0;
    for(const auto& file:std::filesystem::directory_iterator(root))if(file.path().extension()==".fen")
    {
        const auto scene=ReadFrontendScene(Read(file.path()));++files;
        for(auto id:scene.presentation_slides)
        {
            const auto slide=std::find_if(scene.slides.begin(),scene.slides.end(),[&](auto& value){return value.offset==id;});
            if(slide->animated)continue;
            try
            {
                auto result=BuildFrontendLayout(scene,*loc,fonts,id);++frames;entries+=result.TextCount();
                std::cout<<file.path().filename().string()<<'\t'<<slide->name<<'\t'<<result.TextCount()<<'\t'<<result.hidden;
                for(const auto& [reason,count]:result.unavailable)std::cout<<'\t'<<reason<<'='<<count;
                std::cout<<'\n';
            }
            catch(const std::exception& e){std::cout<<file.path().filename().string()<<'\t'<<slide->name<<"\tREJECT\t"<<e.what()<<'\n';}
        }
    }
    Check(files==75&&frames>0&&entries>0,"Owned static frontend audit has no useful qualified text");
    std::cout<<"Owned static layout: "<<files<<" files, "<<frames<<" evaluated static slides, "<<entries<<" retained text entries\n";
}
}
int main(int argc,char** argv)
{
    try{for(unsigned repeat=0;repeat<3;++repeat){Baseline();Inheritance();TextBoxes();ParagraphsAndFallback();Selection();Components();UnsupportedAndErrors();Images();}if(argc==2)Owned(argv[1]);std::cout<<checks<<" original frontend static layout checks passed\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
