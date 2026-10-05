#include "runtime/frontend_pointer.h"
#include "frontend_layout_fixture.h"
#include "frontend_font_fixture.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <source_location>
#include <thread>

thread_local long allocation_budget=-1;
void* operator new(std::size_t n){if(allocation_budget==0)throw std::bad_alloc();if(allocation_budget>0)--allocation_budget;if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace mscharged;using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void Near(float a,float b,float tolerance=.00001f,std::source_location at=std::source_location::current()){++checks;if(!std::isfinite(a)||std::abs(a-b)>=tolerance)throw std::runtime_error("Pointer bounds oracle line "+std::to_string(at.line())+": "+std::to_string(a)+" != "+std::to_string(b));}
template<class F>void Reject(F f,std::source_location at=std::source_location::current())
{++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid pointer operation accepted at "+std::to_string(at.line()));}
std::shared_ptr<FrontendSessionFrame> Images()
{
    auto frame=std::make_shared<FrontendSessionFrame>();auto& s=frame->graph;
    FrontendLibraryObject lib{};lib.offset=10;lib.type=1;lib.attributes.scale={.8f,.6f,1};s.library.push_back(lib);
    for(unsigned n=0;n<2;++n){FrontendInstance v{};v.offset=100+n;v.type=2;v.library=10;v.attributes.scale={2,1,1};v.attributes.position={float(10+100*n),20,0};s.instances.push_back(v);}
    FrontendInstance parent{};parent.offset=200;parent.type=5;parent.attributes.position={40,-10,0};parent.children={100,101};s.instances.push_back(parent);
    FrontendLibraryObject component{};component.offset=20;component.type=3;component.slides={30,31};component.active_slide=30;s.library.push_back(component);
    FrontendSlide slide{};slide.offset=30;slide.children={100};s.slides.push_back(slide);slide.offset=31;slide.children={101};s.slides.push_back(slide);
    FrontendInstance c{};c.offset=300;c.type=4;c.library=20;c.children={200};s.instances.push_back(c);
    return frame;
}
void Bounds()
{
    auto f=Images();auto a=MeasureFrontendPointerBounds(f,{100});Near(a.min_x,-30);Near(a.max_x,50);Near(a.min_y,-10);Near(a.max_y,50);
    f->graph.instances[0].overload_flags=4;
    a=MeasureFrontendPointerBounds(f,{100});Near(a.min_x,-90);Near(a.max_x,110);Near(a.min_y,-30);Near(a.max_y,70);
    // Original asset position ignores library position and the position override bit.
    f->graph.library[0].attributes.position={999,999,0};a=MeasureFrontendPointerBounds(f,{100});Near(a.min_x,-90);
    auto group=MeasureFrontendPointerBounds(f,{200});Near(group.min_x,-80);Near(group.max_x,160);Near(group.min_y,-60);Near(group.max_y,40);
    auto component=MeasureFrontendPointerBounds(f,{300});Near(component.min_x,-100);Near(component.max_x,100);
    f->graph.library[1].active_slide=31;component=MeasureFrontendPointerBounds(f,{300});Near(component.min_x,-40);Near(component.max_x,40);
    f->graph.instances[2].children.clear();group=MeasureFrontendPointerBounds(f,{200});Near(group.min_x,40);Near(group.max_y,-10);
    f->graph.instances[2].children={101};f->graph.instances[1].attributes.position={1000,1000,0};
    group=MeasureFrontendPointerBounds(f,{200});Near(group.max_x-group.min_x,1040-427);Near(group.max_y-group.min_y,1030-240);
    FrontendPointerBinding binding{100};binding.offset_x=13;binding.offset_y=-7;binding.scale_x=.5f;binding.scale_y=2;
    a=MeasureFrontendPointerBounds(f,binding);Near(a.min_x,-27);Near(a.max_x,73);Near(a.min_y,-87);Near(a.max_y,113);
    binding.scale_x=-1;a=MeasureFrontendPointerBounds(f,binding);Check(a.min_x>a.max_x&&!FrontendPointerContains(a,{23,13}),"Negative source size was silently normalized");
    binding={100};binding.use_rotation=true;f->graph.instances[0].attributes.rotation[2]=.73f;binding.offset_x=13;
    a=MeasureFrontendPointerBounds(f,binding);Near(a.pivot[0],10);Near(a.pivot[1],20);Near(a.rotation,.73f);
    // Independent inverse rotation using original signed16 quantization. Points
    // well away from edges avoid treating LUT approximation as exact sin parity.
    const auto angle=double(int(10430.378f*.73f))*6.2831853071795864769/65536;
    for(int x=-150;x<=150;x+=7)for(int y=-150;y<=150;y+=11)
    {
        const double rx=x*std::cos(angle)+y*std::sin(angle)+10;
        const double ry=-x*std::sin(angle)+y*std::cos(angle)+20;
        if(std::min({std::abs(rx-a.min_x),std::abs(rx-a.max_x),std::abs(ry-a.min_y),std::abs(ry-a.max_y)})<.04)continue;
        const bool expected=rx>=a.min_x&&rx<=a.max_x&&ry>=a.min_y&&ry<=a.max_y;
        Check(FrontendPointerContains(a,{float(x+10),float(y+20)})==expected,"Rotated hit oracle differs");
    }
    a={-2,3,-4,5,0,{0,0}};
    for(auto p:std::array<std::array<float,2>,4>{{{-2,-4},{3,-4},{3,5},{-2,5}}})Check(FrontendPointerContains(a,p),"Exact inclusive face was excluded");
    Check(!FrontendPointerContains(a,{std::nextafter(3.f,4.f),0}),"Outside nextafter edge was accepted");
    Reject([&]{FrontendPointerContains(a,{NAN,0});});a.rotation=1e30f;Reject([&]{FrontendPointerContains(a,{0,0});});
    Reject([&]{MeasureFrontendPointerBounds({},{});});Reject([&]{MeasureFrontendPointerBounds(f,{999});});
    f->graph.instances[2].children={200};Reject([&]{MeasureFrontendPointerBounds(f,{200});});
    f->graph.instances[2].children={100,100};Reject([&]{MeasureFrontendPointerBounds(f,{200});});
    f->graph.instances[2].children={};f->graph.library[1].active_slide=999;Reject([&]{MeasureFrontendPointerBounds(f,{300});});
    f->graph.instances.push_back(f->graph.instances[0]);Reject([&]{MeasureFrontendPointerBounds(f,{100});});
    auto chain=Images();chain->graph.instances.resize(1);
    for(unsigned depth=0;depth<65;++depth)
    {
        FrontendInstance node{};node.offset=1000+depth;node.type=5;
        node.children={depth==0?100:999+depth};chain->graph.instances.push_back(node);
    }
    // The image plus63 ancestors fits the native traversal contract exactly.
    (void)MeasureFrontendPointerBounds(chain,{1062});
    Reject([&]{MeasureFrontendPointerBounds(chain,{1063});});
    auto excessive=Images();excessive->graph.instances[2].children.resize(16385,100);
    Reject([&]{MeasureFrontendPointerBounds(excessive,{200});});
}
void Text()
{
    auto font=ReadFrontendFont(font_fixture::Font(),"fe/fonts/fixture","fixture");
    auto f=std::make_shared<FrontendSessionFrame>();f->graph=frontend_layout_fixture::Scene(font->alias);
    f->graph.instances[1].text=u"AB AB";f->graph.instances[1].text_box={30,64};
    f->layout=BuildFrontendLayout(f->graph,Localization{},{&font,1});
    const auto b=MeasureFrontendPointerBounds(f,{300});
    // Original pointer width queries width640, whereas the authored textbox
    // wraps at30: exact width45 (9+10+7+9+10) and two original rows of12, independent of quads.
    Near(b.max_x-b.min_x,45);Near(b.max_y-b.min_y,24);
    auto& text=std::get<FrontendLayoutText>(f->layout.entries.back());
    Check(text.instance==300,"Text fixture reverse order differs");
    text.layout.quads.clear();const auto without_quads=MeasureFrontendPointerBounds(f,{300});Near(without_quads.max_x-without_quads.min_x,45);
    text.layout.height=13;Reject([&]{MeasureFrontendPointerBounds(f,{300});});
    f->layout.entries.clear();Reject([&]{MeasureFrontendPointerBounds(f,{300});});
    f->layout=BuildFrontendLayout(f->graph,Localization{},{&font,1});
    FrontendInput input;FrontendPointerRegion retained(input,f,{300});
    std::weak_ptr<const FrontendFont> weak=font;font.reset();f.reset();
    Check(!weak.expired()&&retained.Contains({-160,120}),"Pointer lost its actual published font/layout after source release");
    retained.Release();Check(weak.expired(),"Pointer release retained the published font/pages");
}
void Events()
{
    FrontendInput input;auto f=Images();std::vector<std::pair<FrontendPointerCallback,unsigned>> log;
    FrontendPointerRegion* self=nullptr;bool fail=false,reentry=false;
    FrontendPointerRegion region(input,f,{100},[&](auto kind,unsigned pad,const auto& frame)
    {
        Check(frame==self->Current()&&self->Enabled(),"Callback query/retained frame differs");log.emplace_back(kind,pad);
        if(reentry){Reject([&]{self->Deliver({});});Reject([&]{self->Disable();});Reject([&]{self->Rebind(f,{100});});Reject([&]{self->Release();});}
        if(fail&&kind==FrontendPointerCallback::Press)throw std::runtime_error("injected callback failure");
    });self=&region;
    const std::array order{FrontendPointerCallback::Enter,FrontendPointerCallback::Update,FrontendPointerCallback::Inside,FrontendPointerCallback::Press,FrontendPointerCallback::Unidentified,FrontendPointerCallback::Release};
    for(unsigned pad=0;pad<4;++pad)
    {
        log.clear();region.Deliver({pad,{10,20},true,true,true});Check(log.size()==6,"Initial inside dispatch lost callbacks");
        for(unsigned i=0;i<6;++i)Check(log[i]==std::pair{order[i],pad},"Original event callback order differs");
        log.clear();region.Deliver({pad,{500,500},false,true});Check(log==std::vector<std::pair<FrontendPointerCallback,unsigned>>{{FrontendPointerCallback::Leave,pad},{FrontendPointerCallback::Release,pad}},"Outside release order differs");
        log.clear();region.Deliver({pad,{500,500},false,true});Check(log.size()==1&&log[0].first==FrontendPointerCallback::Release,"Outside release was hit-test gated");
    }
    region.Deliver({0,{10,20}});region.Disable();log.clear();region.Deliver({0,{500,500},true,true,true});Check(log.empty()&&!region.Enabled(),"Disabled listener dispatched");
    region.Enable();region.Deliver({0,{10,20}});Check(log.front().first==FrontendPointerCallback::Enter,"Disable did not reset previous history");
    int focus=0;input.PushFocus(&focus);input.Focus(&focus);log.clear();region.Deliver({0,{500,500}});Check(log.empty(),"Pointer ignored original nonzero lock depth");input.PopFocus(&focus);
    region.Deliver({0,{500,500}});Check(log.size()==1&&log[0].first==FrontendPointerCallback::Leave,"Locked input changed pointer history");
    input.PushFocus(&focus);region.IgnoreInputLock(true);log.clear();region.Deliver({0,{10,20}});Check(log.front().first==FrontendPointerCallback::Enter,"IgnoreInputLock had no effect");input.PopFocus(&focus);
    region.IgnoreInputLock(false);region.Disable();region.Enable();fail=true;log.clear();Reject([&]{region.Deliver({1,{10,20},true});});fail=false;
    log.clear();region.Deliver({1,{10,20}});Check(log.front().first==FrontendPointerCallback::Enter,"Callback failure committed previous event");
    reentry=true;region.Deliver({2,{10,20}});reentry=false;
    Reject([&]{region.Deliver({4,{0,0}});});Reject([&]{region.Deliver({0,{NAN,0}});});
    auto replacement=Images();replacement->graph.instances[0].attributes.position[0]=200;
    const auto before=region.Bounds();Reject([&]{region.Rebind(replacement,{999});});Check(region.Current()==f&&region.Bounds().min_x==before.min_x,"Failed rebind changed current");
    region.Rebind(replacement,{100});Check(region.Current()==replacement,"Rebind did not publish snapshot");
    Reject([&]{region.Deliver(f,{0,{200,20}});});
    bool wrong=false;std::thread foreign([&]{try{region.Contains({0,0});}catch(const std::exception&){wrong=true;}});foreign.join();Check(wrong,"Foreign thread accessed pointer");
    region.Release();region.Release();Reject([&]{region.Current();});Reject([&]{region.Deliver({});});
}
void OwnershipAndFailure()
{
    FrontendInput input;auto old=Images(),next=Images();next->graph.instances[0].attributes.position[0]=700;
    FrontendPointerRegion owner(input,old,{100});unsigned failures=0;bool success=false;
    for(long budget=0;budget<100&&!success;++budget)
    {
        allocation_budget=budget;bool failed=false;
        try{owner.Rebind(next,{100});}catch(const std::bad_alloc&){failed=true;}allocation_budget=-1;
        if(failed){++failures;Check(owner.Current()==old&&owner.Bounds().min_x==-30,"Allocation failure partially published bounds");}else success=true;
    }
    Check(success&&failures>=6,"Rebind allocation failure sweep missed graph indexing");
    unsigned constructor_failures=0;success=false;
    for(long budget=0;budget<100&&!success;++budget)
    {
        const auto before=next.use_count();allocation_budget=budget;bool failed=false;
        try{FrontendPointerRegion candidate(input,next,{100});}catch(const std::bad_alloc&){failed=true;}
        allocation_budget=-1;
        if(failed)++constructor_failures;else success=true;
        Check(next.use_count()==before,"Partial constructor retained a scene reference");
    }
    Check(success&&constructor_failures>=7,"Constructor allocation sweep missed partial indexing");
    std::weak_ptr<const FrontendSessionFrame> retained=next;next.reset();Check(!retained.expired()&&owner.Contains({700,20}),"Pointer lost source ownership");owner.Release();Check(retained.expired(),"Released pointer retained resources");
    // Source SetBounds retains rotation; no accidental axis-aligned replacement.
    auto rotated=Images();rotated->graph.instances[0].attributes.rotation[2]=.73f;
    FrontendPointerRegion second(input,rotated,{100,true});second.SetBounds(-1,1,1,-1);Near(second.Bounds().rotation,.73f);Near(second.Bounds().pivot[0],10);
    second.Rebind(rotated,{100,false});Near(second.Bounds().rotation,0);Near(second.Bounds().pivot[0],10);
    // A listener may intentionally contain the original sentinel position; no
    // synthetic first-event flag may replace the source previous-position test.
    std::vector<FrontendPointerCallback> log;FrontendPointerRegion sentinel(input,old,{100},[&](auto kind,unsigned,const auto&){log.push_back(kind);});
    sentinel.SetBounds(-20000,20000,20000,-20000);sentinel.Deliver({0,{0,0}});Check(log.front()==FrontendPointerCallback::Update,"Sentinel containment was replaced by invented first-entry semantics");
}
void Owned(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary);Check(bool(file),"Cannot open owned FEN");std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    auto frame=std::make_shared<FrontendSessionFrame>();frame->graph=ReadFrontendScene(bytes);
    unsigned count=0,named=0;
    for(const auto& instance:frame->graph.instances)if(instance.type==2)
    {
        auto scale=instance.attributes.scale;
        if(!(instance.overload_flags&4))
        {const auto it=std::find_if(frame->graph.library.begin(),frame->graph.library.end(),[&](auto& v){return instance.library&&v.offset==*instance.library;});Check(it!=frame->graph.library.end(),"Owned image library missing");scale=it->attributes.scale;}
        const auto b=MeasureFrontendPointerBounds(frame,{instance.offset});
        Near(b.min_x,instance.attributes.position[0]-scale[0]*50,.0002f);Near(b.max_y,instance.attributes.position[1]+scale[1]*50,.0002f);++count;
        if(instance.name=="list_back_480x70 ")++named;
    }
    Check(count>0&&named>0,"Owned options pointer target was not audited");
    std::cout<<"Owned options: "<<count<<" image bounds, "<<named<<" original list_back_480x70 targets\n";
}
}
int main(int argc,char** argv)
{
    try{for(int n=0;n<3;++n){Bounds();Text();Events();OwnershipAndFailure();}if(argc==2)Owned(argv[1]);else Check(argc==1,"Supply optional owned options.fen");std::cout<<checks<<" frontend pointer checks passed\n";}
    catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
