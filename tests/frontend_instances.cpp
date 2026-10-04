#include "resources/frontend_instances.h"
#include "resources/frontend_fonts.h"
#include "frontend_layout_fixture.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
using namespace mscharged::resources;
namespace {
unsigned checks=0;
void Check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid instance operation accepted at line "+std::to_string(at.line()));}
FrontendPath Path(std::initializer_list<std::string_view> names){return FrontendNamedPath(names);}
FrontendScene Graph()
{
    auto s=frontend_layout_fixture::Scene(123);
    for(auto& item:s.instances)item.hash=FrontendLowerHash(item.name);
    s.slides[0].hash=FrontendLowerHash("main");s.instances[0].name="Layer";s.instances[0].hash=FrontendLowerHash("layer");
    // Two same-hash siblings deliberately retain original first-ring semantics.
    s.instances[2].hash=s.instances[1].hash;
    FrontendLibraryObject c{};c.offset=600;c.type=3;c.slides={700,701};c.active_slide=700;s.library.push_back(c);
    FrontendInstance component{};component.offset=610;component.type=4;component.hash=FrontendLowerHash("component");component.library=600;component.children={500};s.instances.push_back(component);
    s.instances[0].children.push_back(610);
    FrontendSlide in{};in.offset=700;in.hash=FrontendLowerHash("in");in.children={800};s.slides.push_back(in);
    FrontendSlide out{};out.offset=701;out.hash=FrontendLowerHash("out");out.children={801};s.slides.push_back(out);
    for(unsigned i=0;i<2;++i){FrontendInstance label{};label.offset=800+i;label.type=3;label.hash=FrontendLowerHash("label");s.instances.push_back(label);}
    return s;
}
void Lookup()
{
    const auto s=Graph();
    Check(FrontendLowerHash("ABC")==0x00011d45u,"Name hash is not original lowercase hash");
    const auto find=[&](FrontendNode root,FrontendPath path,FrontendNodeType type=FrontendNodeType::Any){return FindFrontendNode(s,root,path,type);};
    Check(find({},Path({"MAIN"}))==FrontendNode{FrontendNodeKind::Slide,100},"Presentation slide lookup differs");
    Check(find({},Path({"Layer","Text0"}))==FrontendNode{FrontendNodeKind::Instance,300},"Current-slide fallback or first hash match differs");
    Check(find({},Path({"main","layer","text0"}),FrontendNodeType::Text)==FrontendNode{FrontendNodeKind::Instance,300},"Named slide path differs");
    Check(find({},Path({"layer","component","in"}))==FrontendNode{FrontendNodeKind::Slide,700},"Component named slide lookup differs");
    Check(find({},Path({"layer","component","label"}))==FrontendNode{FrontendNodeKind::Instance,800},"Component active-slide fallback differs");
    Check(find({},Path({"main","layer","component","out","label"}))==FrontendNode{FrontendNodeKind::Instance,801},"Nested explicit slide path differs");
    Check(find({FrontendNodeKind::Instance,610},Path({"label"}))==FrontendNode{FrontendNodeKind::Instance,800},"Instance root did not use original component semantics");
    Check(!find({FrontendNodeKind::Instance,610},Path({"Text2"})),"Component incorrectly searched its instance child ring");
    Check(find({FrontendNodeKind::Slide,100},Path({"layer","Text0"}))==FrontendNode{FrontendNodeKind::Instance,300},"Slide root differs");
    auto path=Path({"layer","text0"});path[1]=0;path[2]=0xdead;
    Check(find({},path)==FrontendNode{FrontendNodeKind::Instance,200},"Zero next path slot did not stop traversal");
    Check(!find({},Path({"absent"})),"Missing lookup did not return empty");
    Reject([&]{find({},Path({"layer","text0"}),FrontendNodeType::Image);});
    Reject([&]{find({FrontendNodeKind::Slide,888},Path({"a"}));});
    Reject([&]{FrontendNamedPath({});});Reject([&]{Path({"a","b","c","d","e","f","g"});});
    Reject([&]{FrontendLowerHash(std::string("a\0b",3));});
    auto broken=s;broken.instances[0].children.insert(broken.instances[0].children.begin(),999);
    Reject([&]{FindFrontendNode(broken,{},Path({"layer","text0"}));});
    broken=s;broken.library.back().active_slide=888;Reject([&]{FindFrontendNode(broken,{},Path({"layer","component","label"}));});
    broken=s;broken.slides.push_back(broken.slides.front());Reject([&]{FindFrontendNode(broken,{},Path({"main"}));});
    broken=s;broken.active_slide.reset();Check(!FindFrontendNode(broken,{},Path({"layer"})),"Null presentation active slide fabricated children");
    // Named slides take precedence even if a child has the same hash.
    broken=s;broken.slides[0].hash=FrontendLowerHash("layer");Check(FindFrontendNode(broken,{},Path({"layer"}))==FrontendNode{FrontendNodeKind::Slide,100},"Child won over named presentation slide");
}
FrontendInstanceChange Change(FrontendInstanceProperty p,unsigned id=300){FrontendInstanceChange c;c.instance=id;c.property=p;return c;}
void Mutation()
{
    auto s=Graph();auto& before=s.instances[1];before.attributes.colour={17,29,41,77};before.overload_flags=0x8000;before.text_overload_flags=0xf0;
    auto visible=Change(FrontendInstanceProperty::Visible);visible.flag=false;ApplyFrontendInstanceChanges(s,{&visible,1});
    Check(!s.instances[1].visible&&s.instances[1].attributes.colour[3]==77&&s.instances[1].overload_flags==0x8000,"SetVisible changed asset alpha/flags");
    auto alpha=Change(FrontendInstanceProperty::AssetVisible);alpha.flag=false;ApplyFrontendInstanceChanges(s,{&alpha,1});
    Check(s.instances[1].attributes.colour==std::array<std::uint8_t,4>{17,29,41,0}&&s.instances[1].overload_flags==0x8010,"AssetVisible did not preserve raw RGB");
    alpha.flag=true;ApplyFrontendInstanceChanges(s,{&alpha,1});Check(s.instances[1].attributes.colour[3]==255&&!s.instances[1].visible,"Asset alpha incorrectly enabled visibility");
    for(auto p:{FrontendInstanceProperty::Position,FrontendInstanceProperty::Rotation,FrontendInstanceProperty::Scale,FrontendInstanceProperty::Pivot})
    {auto c=Change(p);c.vector={-3,2,.5};ApplyFrontendInstanceChanges(s,{&c,1});}
    Check((s.instances[1].overload_flags&31)==31&&s.instances[1].attributes.position==std::array<float,3>{-3,2,.5}
        &&s.instances[1].attributes.rotation==s.instances[1].attributes.position&&s.instances[1].attributes.scale==s.instances[1].attributes.position
        &&s.instances[1].attributes.pivot==s.instances[1].attributes.position,"Vector setters lost fields/override masks");
    unsigned i=0;for(auto p:{FrontendInstanceProperty::UVX,FrontendInstanceProperty::UVY,FrontendInstanceProperty::UVWidth,FrontendInstanceProperty::UVHeight})
    {auto c=Change(p);c.scalar=float(++i)*.25;ApplyFrontendInstanceChanges(s,{&c,1});}
    Check(s.instances[1].attributes.uv==std::array<float,4>{.25,.5,.75,1}&&(s.instances[1].overload_flags&0x3c0)==0x3c0,"UV setter mapping differs");
    auto id=Change(FrontendInstanceProperty::StringId);id.string_id="LOC_Test";ApplyFrontendInstanceChanges(s,{&id,1});
    Check(s.instances[1].localization_hash==FrontendLowerHash("test")&&s.instances[1].text_overload_flags==0xf8,"String ID prefix/flags differ");
    id.string_id="loc_Test";ApplyFrontendInstanceChanges(s,{&id,1});Check(s.instances[1].localization_hash==FrontendLowerHash("loc_test"),"Lowercase LOC_ incorrectly stripped");
    auto text=Change(FrontendInstanceProperty::String);text.text=u"AB";ApplyFrontendInstanceChanges(s,{&text,1});text.text.clear();
    Check(s.instances[1].text==u"AB"&&s.instances[1].text_overload_flags==0xf0,"SetString failed ownership or cleared unrelated flags");
    auto image=s.instances[1];image.offset=900;image.type=2;image.resource=901;s.instances.push_back(image);s.resources.push_back({901,0,1234,0,false});s.resources.push_back({902,0,5678,0,false});
    auto resource=Change(FrontendInstanceProperty::ImageResource,900);ApplyFrontendInstanceChanges(s,{&resource,1});Check(s.instances.back().resource==901,"Null image setter did not retain original resource");
    resource.image_resource=902;ApplyFrontendInstanceChanges(s,{&resource,1});Check(s.instances.back().resource==902,"Image resource binding differs");
    auto bad=Change(FrontendInstanceProperty::Position);bad.vector[1]=NAN;
    const auto old=s.instances[1].attributes.position;std::array edits{visible,bad};edits[0].flag=true;
    Reject([&]{ApplyFrontendInstanceChanges(s,edits);});Check(!s.instances[1].visible&&s.instances[1].attributes.position==old,"Failed batch partially changed graph");
    resource.instance=300;Reject([&]{ApplyFrontendInstanceChanges(s,{&resource,1});});
    resource.instance=900;resource.image_resource=10;Reject([&]{ApplyFrontendInstanceChanges(s,{&resource,1});});
    text.text=std::u16string(u"A\0B",3);Reject([&]{ApplyFrontendInstanceChanges(s,{&text,1});});
    visible.instance=0xdead;Reject([&]{ApplyFrontendInstanceChanges(s,{&visible,1});});
    std::vector<FrontendInstanceChange> excessive(257,alpha);Reject([&]{ApplyFrontendInstanceChanges(s,excessive);});
}
void Owned(const std::filesystem::path& folder)
{
    std::size_t files=0,instances=0;
    for(const auto& entry:std::filesystem::directory_iterator(folder))if(entry.path().extension()==".fen")
    {
        std::ifstream file(entry.path(),std::ios::binary);std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
        const auto scene=ReadFrontendScene(bytes);++files;
        for(const auto& slide:scene.slides)for(auto id:slide.children)
        {
            const auto it=std::find_if(scene.instances.begin(),scene.instances.end(),[&](auto& v){return v.offset==id;});
            Check(it!=scene.instances.end(),"Owned direct child absent");FrontendPath path{};path[0]=it->hash;
            const auto found=FindFrontendNode(scene,{FrontendNodeKind::Slide,slide.offset},path);
            Check(found&&found->kind==FrontendNodeKind::Instance,"Owned direct hash lookup failed");++instances;
        }
    }
    Check(files==75&&instances>100,"Owned finder audit is incomplete");std::cout<<"Owned finder: "<<files<<" files, "<<instances<<" direct slide child lookups\n";
}
}
int main(int argc,char** argv)
{
    try{for(unsigned n=0;n<3;++n){Lookup();Mutation();}if(argc==2)Owned(argv[1]);std::cout<<checks<<" frontend instance checks passed\n";}
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
