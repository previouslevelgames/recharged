#include "resources/frontend_animation.h"
#include "resources/frontend_layout.h"
#include "frontend_layout_fixture.h"
#include "frontend_image_fixture.h"
#include "frontend_font_fixture.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void Near(float a,float b,float tolerance=.0002f){Check(std::isfinite(a)&&std::abs(a-b)<=tolerance,"Animation analytic oracle differs");}
template<class F>void Reject(F fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid animation accepted");}
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read animation fixture");return {std::istreambuf_iterator<char>(f),{}};}
FrontendAnimation Track(unsigned id,unsigned target,unsigned type,std::array<float,3> from,std::array<float,3> to,float first=0,float last=1)
{
    FrontendAnimation a;a.offset=id;a.target=target;a.type=type;a.cast=type<=5;
    for(unsigned i=0;i<2;++i)
    {
        FrontendAnimationKey key;key.offset=id+1+i;
        for(unsigned c=0;c<(a.cast?3u:1u);++c)key.channels[c]=i?std::array<float,4>{to[c],-1,-1,last}
            :std::array<float,4>{from[c],from[c]+(to[c]-from[c])/3,from[c]+2*(to[c]-from[c])/3,first};
        a.keys.push_back(key);
    }
    return a;
}
FrontendScene Scene()
{
    auto scene=frontend_layout_fixture::Scene(123);frontend_image_fixture::AddImage(scene,99);
    scene.slides[0].duration=1;scene.slides[0].play_mode=1;
    scene.animations.push_back(Track(2000,600,1,{-160,0,0},{160,0,0}));
    scene.slides[0].animated=true;scene.slides[0].animations={2000};return scene;
}
const FrontendInstance& Target(const FrontendScene& scene,unsigned id=600)
{const auto it=std::find_if(scene.instances.begin(),scene.instances.end(),[&](auto& i){return i.offset==id;});if(it==scene.instances.end())throw std::runtime_error("Missing test target");return *it;}
void Clock()
{
    auto source=Scene();FrontendAnimationPlayback p(source);Near(Target(p.Scene()).attributes.position[0],-160);Near(p.PresentationTime(),0);
    Check(!source.slides[0].animation_evaluated&&p.Scene().slides[0].animation_evaluated,"Playback did not isolate evaluated ownership");
    p.Advance(.125f);Near(p.PresentationTime(),.125f);Near(p.Scene().slides[0].time,.25f);Near(Target(p.Scene()).attributes.position[0],-80);
    p.Reset();p.Advance(.5f);Near(p.Scene().slides[0].time,1);Near(Target(p.Scene()).attributes.position[0],160);
    p.Advance(.5f);Near(p.PresentationTime(),1);Near(p.Scene().slides[0].time,.5f);Near(Target(p.Scene()).attributes.position[0],0);
    p.Reset();p.Advance(2.5f);Near(p.PresentationTime(),1.5f);Near(p.Scene().slides[0].time,3);Near(Target(p.Scene()).attributes.position[0],160);
    source.slides[0].play_mode=0;FrontendAnimationPlayback stop(source);stop.Advance(4);Near(stop.PresentationTime(),1);Near(stop.Scene().slides[0].time,1);
    source.slides[0].play_mode=2;FrontendAnimationPlayback continuous(source);continuous.Advance(2);Near(continuous.PresentationTime(),2);Near(continuous.Scene().slides[0].time,4);
    source.slides[0].frozen=true;FrontendAnimationPlayback frozen(source);frozen.Advance(.25f);Near(frozen.PresentationTime(),.25f);Near(frozen.Scene().slides[0].time,.25f);
    source.slides[0].start=.5f;source.slides[0].duration=.5f;source.slides[0].play_mode=1;source.slides[0].frozen=false;
    FrontendAnimationPlayback offset(source);offset.Advance(.5f);Near(offset.Scene().slides[0].time,1);
    p.Reset();for(unsigned i=0;i<120;++i)p.Advance(1.f/60);auto first=Target(p.Scene()).attributes.position;
    p.Reset();for(unsigned i=0;i<120;++i)p.Advance(1.f/60);Check(Target(p.Scene()).attributes.position==first,"Reset did not replay deterministically");
}
void Channels()
{
    for(unsigned type=1;type<=10;++type)
    {
        auto s=Scene();s.animations[0]=Track(2000,600,type,{12,18,24},{36,42,48});FrontendAnimationPlayback p(s);p.Advance(.125f);
        const auto& t=Target(p.Scene());
        if(type<=4)
        {const auto& v=type==1?t.attributes.position:type==2?t.attributes.rotation:type==3?t.attributes.scale:t.attributes.pivot;for(unsigned c=0;c<3;++c)Near(v[c],18+6*c);Check(t.overload_flags&(1u<<(type-1)),"Vector setter override flag changed");}
        else if(type==5)Check(t.attributes.colour==std::array<std::uint8_t,4>{18,24,30,255},"Colour setter did not force opaque alpha");
        else if(type==6)Check(t.attributes.colour==std::array<std::uint8_t,4>{255,255,255,18},"Opacity setter lost original overloaded RGB");
        else{Near(t.attributes.uv[type-7],18);Check(t.overload_flags&(0x40u<<(type-7)),"Scalar setter override flag changed");}
    }
    auto s=Scene();s.animations[0]=Track(2000,600,6,{0,0,0},{100,0,0},.5f,1);s.instances.back().attributes.colour[3]=42;
    FrontendAnimationPlayback before(s);Check(Target(before.Scene()).attributes.colour[3]==42,"Scalar before-first key changed target");
    s.animations[0].keys.resize(1);FrontendAnimationPlayback single(s);single.Advance(.5f);Check(Target(single.Scene()).attributes.colour[3]==42,"Original scalar singleton wrote a value");
    s.animations[0]=Track(2000,600,1,{12,18,0},{36,42,0},.5f,1);FrontendAnimationPlayback clamped(s);Near(Target(clamped.Scene()).attributes.position[0],12);
    s.animations[0].keys.resize(1);s.animations[0].keys[0].channels[0][1]=s.animations[0].keys[0].channels[0][2]=-1;
    FrontendAnimationPlayback vector_single(s);vector_single.Advance(.5f);Near(Target(vector_single.Scene()).attributes.position[0],12);
    s.animations[0]=Track(2000,600,2,{0,0,0},{0,0,1});s.animations[0].keys[0].channels[0][1]=-1;s.animations[0].keys[0].channels[0][2]=-1;
    FrontendAnimationPlayback rotation_hold(s);rotation_hold.Advance(.25f);Near(Target(rotation_hold.Scene()).attributes.rotation[2],0);
    s=Scene();s.animations={Track(2000,600,5,{10,20,30},{10,20,30}),Track(2010,600,6,{64,0,0},{64,0,0})};s.slides[0].animations={2000,2010};
    FrontendAnimationPlayback colour_then_opacity(s);Check(Target(colour_then_opacity.Scene()).attributes.colour[3]==64,"Ordered opacity was lost");
    std::reverse(s.slides[0].animations.begin(),s.slides[0].animations.end());FrontendAnimationPlayback opacity_then_colour(s);Check(Target(opacity_then_colour.Scene()).attributes.colour[3]==255,"Ordered colour alpha reset was lost");
    s=Scene();s.animations[0]=Track(2000,600,6,{-1,0,0},{-1,0,0});s.instances.back().attributes.colour[3]=99;
    FrontendAnimationPlayback sentinel(s);Check(Target(sentinel.Scene()).attributes.colour[3]==99,"Exact opacity -1 sentinel wrote a value");
    // Independent cubic Bernstein oracle (nonlinear outgoing controls).
    s=Scene();s.animations[0].keys[0].channels[0]={10,80,-20,0};s.animations[0].keys[1].channels[0][0]=30;
    for(float time:{.125f,.25f,.5f,.75f,.875f})
    {FrontendAnimationPlayback p(s);p.Advance(time/2);const double t=time,u=1-t;Near(Target(p.Scene()).attributes.position[0],float(10*u*u*u+3*80*u*u*t-3*20*u*t*t+30*t*t*t),.001f);}
}
void Nested()
{
    auto s=Scene();auto nested=s.slides[0];nested.offset=101;nested.time=.25f;nested.children={600};s.slides.push_back(nested);
    s.slides[0].animated=false;s.slides[0].animations.clear();s.slides[0].children={700,701};
    FrontendLibraryObject lib{};lib.offset=800;lib.type=3;lib.active_slide=101;lib.slides={101};s.library.push_back(lib);
    for(unsigned id:{700u,701u}){FrontendInstance i{};i.offset=id;i.type=4;i.library=800;i.visible=false;i.start=100;i.duration=0;s.instances.push_back(i);}
    FrontendAnimationPlayback p(s);Near(p.Scene().slides[1].time,.25f);Check(p.ChannelsEvaluated()==2,"Shared component slide was not visited twice");
    p.Advance(.125f);Near(p.Scene().slides[1].time,.5f);Near(Target(p.Scene()).attributes.position[0],0);
    p.Reset();Near(p.Scene().slides[1].time,.25f); // Restore exported nested time; no fabricated global reset.
    s.slides[0].frozen=true;FrontendAnimationPlayback frozen(s);frozen.Advance(.5f);Near(frozen.Scene().slides[1].time,.25f);
    s.slides[1].children={700};Reject([&]{FrontendAnimationPlayback bad(s);});
}
void Failure()
{
    auto s=Scene();s.animations[0]=Track(2000,600,6,{0,0,0},{255,0,0});s.animations[0].keys[0].channels[0][1]=s.animations[0].keys[0].channels[0][2]=1000;
    FrontendAnimationPlayback p(s);const auto old=Target(p.Scene()).attributes.colour;Reject([&]{p.Advance(.25f);});Near(p.PresentationTime(),0);
    Check(Target(p.Scene()).attributes.colour==old&&p.ChannelsEvaluated()==1,"Failed update partially published");p.Reset();Near(p.PresentationTime(),0);
    for(float dt:{-1.f,61.f,NAN,INFINITY})Reject([&]{p.Advance(dt);});
    auto singleton=Scene();singleton.animations[0].keys.resize(1);FrontendAnimationPlayback single(singleton);
    Reject([&]{single.Advance(.25f);});Near(single.PresentationTime(),0);
    for(unsigned mode=0;mode<9;++mode)
    {
        auto bad=Scene();switch(mode){case 0:bad.animations[0].target=999;break;case 1:bad.animations[0].cast=2;break;
        case 2:bad.animations[0].type=99;break;case 3:bad.animations[0].keys.clear();break;case 4:bad.animations[0].keys[1].channels[0][3]=0;break;
        case 5:bad.animations[0].keys[1].channels[1][3]=2;break;case 6:bad.animations[0].keys[0].channels[0][0]=NAN;break;
        case 7:bad.slides[0].animations.push_back(2000);break;case 8:bad.animations[0].keys[0].offset=600;break;}
        Reject([&]{FrontendAnimationPlayback invalid(bad);});
    }
    s=Scene();Reject([&]{FrontendAnimationPlayback wrong(s,999);});s.active_slide.reset();FrontendAnimationPlayback idle(s);idle.Advance(1);Check(idle.ChannelsEvaluated()==0,"Absent active slide invented playback");
}
void WorkBudget()
{
    auto s=Scene();auto nested=s.slides[0];nested.offset=101;nested.duration=10000;nested.children={600};s.slides.push_back(nested);
    s.slides[0].animated=false;s.slides[0].animations.clear();s.slides[0].children.clear();
    FrontendLibraryObject lib{};lib.offset=8000;lib.type=3;lib.active_slide=101;lib.slides={101};s.library.push_back(lib);
    for(unsigned i=0;i<257;++i)
    {FrontendInstance instance{};instance.offset=7000+i;instance.type=4;instance.library=8000;s.instances.push_back(instance);s.slides[0].children.push_back(instance.offset);}
    auto& track=s.animations[0];track.cast=0;track.type=6;track.keys.clear();
    for(unsigned i=0;i<4096;++i)
    {FrontendAnimationKey key;key.offset=50000+i;key.channels[0]={127,127,127,float(i)};track.keys.push_back(key);}
    auto owner=std::make_unique<FrontendAnimationPlayback>(Scene());owner->Advance(.125f);
    const auto before=Target(owner->Scene()).attributes.position;bool rejected=false;
    try{owner=std::make_unique<FrontendAnimationPlayback>(s);}
    catch(const std::exception& e){rejected=std::string(e.what()).find("sampled-key work budget")!=std::string::npos;}
    Check(rejected,"Repeated shared components did not exhaust the sampled-key budget");
    Check(Target(owner->Scene()).attributes.position==before&&owner->PresentationTime()==.125f,
        "Failed oversized playback replaced the retained owner");
    Check(Target(s).attributes.colour[3]==255,"Rejected playback mutated its source graph");
}
void Selection()
{
    auto source=Scene();source.slides[0].hash=FrontendNameHash("main");
    auto second=source.slides[0];second.offset=101;second.hash=FrontendNameHash("other");second.animations.clear();second.animated=false;second.children.clear();second.time=.75f;
    source.slides.push_back(second);source.presentation_slides.push_back(101);
    auto duplicate=second;duplicate.offset=102;source.slides.push_back(duplicate);source.presentation_slides.push_back(102);
    FrontendAnimationPlayback p(source);p.Advance(.125f);auto before=Target(p.Scene()).attributes.position;
    auto clone=p.Clone();Check(clone->SelectPresentation("MAIN"),"Case-insensitive selection failed");Near(clone->PresentationTime(),.125f);
    Check(clone->SelectPresentation("Main",true),"Forced selection failed");Near(clone->PresentationTime(),0);
    Check(Target(clone->Scene()).attributes.position==before,"Presentation selection ran Update(0)");
    Near(p.PresentationTime(),.125f);Check(Target(p.Scene()).attributes.position==before,"Clone mutated original owner");
    Check(clone->SelectPresentation("Other")&&clone->Scene().active_slide==101,"Original first hash match changed");
    Check(!clone->SelectPresentation("missing")&&!clone->Scene().active_slide,"Missing presentation did not clear");
    clone->Advance(.25f);Near(clone->PresentationTime(),0);clone->Reset();Check(clone->Scene().active_slide==100,"Reset lost selected exported baseline");
    Reject([&]{clone->SelectPresentation(std::string("ma\0in",5));});
    // Component selection runs only the selected child slide at zero delta;
    // preserve_time wins even when forced or changing active slide.
    source=Scene();source.slides[0].animated=false;source.slides[0].animations.clear();source.slides[0].children.clear();
    auto nested=source.slides[0];nested.offset=101;nested.hash=FrontendNameHash("in");nested.time=.25f;nested.animated=true;nested.animations={2000};nested.children={600};
    source.slides.push_back(nested);auto out=nested;out.offset=102;out.hash=FrontendNameHash("out");out.time=.75f;out.animations.clear();out.animated=false;out.children.clear();source.slides.push_back(out);
    FrontendLibraryObject component{};component.offset=800;component.type=3;component.active_slide=101;component.slides={101,102};source.library.push_back(component);
    FrontendAnimationPlayback child(source);
    const auto time=[&](unsigned n){return child.Scene().slides[n].time;};
    Check(child.SelectComponent(800,"IN"),"Component lowercase lookup failed");Near(time(1),.25f);Near(Target(child.Scene()).attributes.position[0],-80);
    Check(child.SelectComponent(800,"in",true,true),"Forced-preserved component selection failed");Near(time(1),.25f);
    Check(child.SelectComponent(800,"out",false,true),"Changed-preserved component selection failed");Near(time(2),.75f);
    Check(child.SelectComponent(800,"in"),"Changed reset component selection failed");Near(time(1),0);Near(Target(child.Scene()).attributes.position[0],-160);
    Check(!child.SelectComponent(800,"missing")&&!child.Scene().library.back().active_slide,"Missing component slide did not clear");
    Reject([&]{child.SelectComponent(12345,"in");});Reject([&]{child.SelectComponent(source.library[0].offset,"in");});
    // Failure after a real selected-slide reset and sample must restore identity,
    // time and attributes together.
    source.animations[0]=Track(2000,600,5,{300,0,0},{300,0,0});source.library.back().active_slide=102;
    FrontendAnimationPlayback failed(source);Reject([&]{failed.SelectComponent(800,"in");});
    Check(failed.Scene().library.back().active_slide==102&&failed.Scene().slides[1].time==.25f
        &&Target(failed.Scene()).attributes.colour[0]==255,"Failed component selection partially changed state");
}
void LoadingSetup()
{
    auto s=Scene();auto& layer=s.instances.front();layer.hash=FrontendLowerHash("layer");layer.children={610};
    auto& top=s.slides.front();top.children={layer.offset};top.animations.clear();top.animated=false;
    FrontendLibraryObject library{};library.offset=800;library.type=3;library.active_slide=102;library.slides={101,102};s.library.push_back(library);
    FrontendInstance component{};component.offset=610;component.type=4;component.hash=FrontendLowerHash("no home");component.library=800;component.visible=true;component.duration=100;s.instances.push_back(component);
    FrontendSlide wide{};wide.offset=101;wide.hash=FrontendLowerHash("widescreen");wide.duration=1;wide.time=.25f;wide.animated=true;wide.animations={2000};wide.children={600};s.slides.push_back(wide);
    FrontendSlide regular{};regular.offset=102;regular.hash=FrontendLowerHash("slide1");regular.duration=1;s.slides.push_back(regular);
    FrontendAnimationPlayback p(s);auto before=p.Clone();
    auto result=p.SetupLoadingScene(false);Check(result.transition_component==610&&!result.widescreen&&!p.Scene().instances.back().visible,"Original loading setup did not hide transition");
    Check(p.Scene().library.back().active_slide==102,"Regular loading setup changed component selection");
    result=p.SetupLoadingScene(true);Check(result.widescreen&&p.Scene().library.back().active_slide==101,"Wide loading setup did not select authored slide");
    Near(p.Scene().slides[1].time,0);Near(Target(p.Scene()).attributes.position[0],-160);
    Check(before->Scene().instances.back().visible&&before->Scene().library.back().active_slide==102,"Setup mutated retained clone");
    result=p.SetupLoadingScene(false);Check(result.widescreen,"Repeated setup lost original sticky widescreen state");
    p.Reset();Check(p.Scene().instances.back().visible,"Reset did not restore setup visibility");
    s.slides[1].hash=123;FrontendAnimationPlayback missing(s);Reject([&]{missing.SetupLoadingScene(true);});
    Check(missing.Scene().instances.back().visible&&missing.Scene().library.back().active_slide==102,"Failed authored setup published partial hide/selection");
    s.instances.back().hash=123;FrontendAnimationPlayback absent(s);Reject([&]{absent.SetupLoadingScene(false);});
    // An ordinary handler setter remains until the original animation channel
    // next writes it; setters never rewrite the exported Reset baseline.
    FrontendInstanceChange edit;edit.instance=600;edit.property=FrontendInstanceProperty::Position;edit.vector={99,7,0};
    p.Apply({&edit,1});Near(Target(p.Scene()).attributes.position[0],99);p.SetupLoadingScene(true);Near(Target(p.Scene()).attributes.position[0],-160);
}
void File(const std::filesystem::path& file)
{
    auto s=ReadFrontendScene(Read(file));Check(s.animations.size()==1&&s.animations[0].keys.size()==2,"Generated FEN animation decode failed");
    FrontendAnimationPlayback p(s);Near(Target(p.Scene(),0x80).attributes.position[0],-160);p.Advance(.5f);Near(Target(p.Scene(),0x80).attributes.position[0],0);
}
void Owned(const std::filesystem::path& root)
{
    unsigned animations=0,keys=0,files=0;
    for(const auto& f:std::filesystem::directory_iterator(root))if(f.path().extension()==".fen")
    {const auto s=ReadFrontendScene(Read(f.path()));++files;animations+=s.animations.size();for(const auto& a:s.animations)keys+=a.keys.size();}
    Check(files==75&&animations==2310&&keys==15007,"Owned animation inventory differs");
    const auto s=ReadFrontendScene(Read(root/"game_summary.fen"));FrontendReference selected;
    for(const auto& slide:s.slides)if(slide.name=="Slide1"&&std::find(s.presentation_slides.begin(),s.presentation_slides.end(),slide.offset)!=s.presentation_slides.end())selected=slide.offset;
    Check(selected.has_value(),"Owned presentation Slide1 is absent");FrontendAnimationPlayback p(s,selected);
    Near(Target(p.Scene(),18460).attributes.position[1],400);
    for(unsigned i=0;i<120;++i)p.Advance(1.f/60);
    const float nested=std::find_if(p.Scene().slides.begin(),p.Scene().slides.end(),[](auto& slide){return slide.offset==18388;})->time;
    const double mu=(nested-double(.8f))/(double(4.8f)-double(.8f)),u=1-mu;
    Near(Target(p.Scene(),18460).attributes.position[1],float(400*u*u*u+3*double(183.355f)*u*u*mu+3*double(-33.3355f)*u*mu*mu-250*mu*mu*mu),.002f);
    auto first=Target(p.Scene(),18460).attributes.position;for(unsigned i=0;i<200;++i)p.Advance(1.f/60);
    Check(Target(p.Scene(),18460).attributes.position!=first,"Owned panel did not move and loop");p.Reset();Near(Target(p.Scene(),18460).attributes.position[1],400);
    std::cout<<"Owned animation: "<<files<<" files, "<<animations<<" channels, "<<keys<<" keys; game_summary blackbox4 source curve/loop/reset passed\n";
}
}
int main(int argc,char** argv)
{
    try{for(unsigned repeat=0;repeat<3;++repeat){Clock();Channels();Nested();Failure();WorkBudget();Selection();LoadingSetup();}
        if(argc==3&&std::string(argv[1])=="--file")File(argv[2]);else if(argc==2)Owned(argv[1]);
        std::cout<<checks<<" frontend animation checks passed\n";
    }catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
