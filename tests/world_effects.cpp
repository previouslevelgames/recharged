#include "runtime/world_effects.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/Effects/ParticleSystem.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks=0;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void Reject(F fn,std::source_location at=std::source_location::current())
{++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("Invalid world-effect operation accepted at "+std::to_string(at.line()));}
std::vector<std::uint8_t> Read(const std::filesystem::path& p)
{std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("Missing world-effect test input");return{std::istreambuf_iterator<char>(f),{}};}
void Set(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t value)
{for(unsigned i=0;i<4;++i)b.at(at+i)=value>>(24-8*i);}
std::uint32_t Step(std::uint32_t seed){const auto a=seed^0x1d872b41U,b=a^(a>>5);return b^a^(b<<27);}
auto Planes(){return std::array<std::array<float,4>,6>{{{1,0,0,1000},{-1,0,0,1000},{0,1,0,1000},{0,-1,0,1000},{0,0,1,1000},{0,0,-1,1000}}};}
std::uint64_t Fingerprint(const resources::WorldEffectData& data)
{
    std::uint64_t hash=14695981039346656037ULL;
    auto word=[&](std::uint32_t n){for(int shift=24;shift>=0;shift-=8){hash^=(n>>shift)&255;hash*=1099511628211ULL;}};
    for(const auto& e:data.Records())
    {
        for(auto n:{e.id,e.group,e.creation_flags,e.probability,std::uint32_t(e.count),std::uint32_t(e.timing),std::uint32_t(e.animation_node),std::bit_cast<std::uint32_t>(e.interval),std::bit_cast<std::uint32_t>(e.repeat_offset)})word(n);
        for(float f:e.matrix)word(std::bit_cast<std::uint32_t>(f));word(e.animated);word(e.always_visible);
    }
    return hash;
}
struct Recording final:WorldEffectEmitter
{
    WorldEffects* world=nullptr;bool fail=false,fail_release=false;unsigned released=0;
    std::vector<std::uint32_t> order;std::vector<std::function<WorldEffectFrame()>> callbacks;
    WorldEffectEmission Emit(const WorldEffectRequest& r)override
    {
        Check(r.owner&&r.group==0x81f2a311&&!r.animated,"Typed source request differs");
        if(world){Reject([&]{world->Trigger(39);});Reject([&]{world->Release();});}
        if(fail)throw std::runtime_error("Controlled emission admission failure");
        callbacks.push_back(r.current);order.push_back(r.object);return{int(order.size()),2};
    }
    void Release(std::uint64_t identity)override
    {
        Check(identity!=0,"Missing owner identity");if(world)Reject([&]{world->Reset();});
        if(fail_release)throw std::runtime_error("Controlled service release failure");callbacks.clear();++released;
    }
};
void Generated(const std::filesystem::path& folder)
{
    const auto bytes=Read(folder/"world.res");auto data=resources::WorldEffectData::Decode(bytes);Check(data->Records().size()==3,"World effects lost records");
    std::cout<<"world-effects-fingerprint="<<std::hex<<Fingerprint(*data)<<std::dec<<'\n';
    Check(data->Records()[0].id==1&&data->Records()[0].matrix[15]==1&&!data->Records()[0].animated,"Disk state/pointers were interpreted incorrectly");
    for(auto [offset,value]:std::vector<std::pair<unsigned,unsigned>>{{32+12,3},{32+0x20+12,0x3f800000},{32+0x60,0xbf800000},{32+0x74,101},{32+0x78,0xfffffffe},{32+0x9c,0x02000000},{32+0x20,0x7fc00000}})
    {auto bad=bytes;Set(bad,offset,value);Reject([&]{resources::WorldEffectData::Decode(bad);});}
    auto truncated=bytes;truncated.pop_back();Reject([&]{resources::WorldEffectData::Decode(truncated);});
    auto duplicate=bytes;Set(duplicate,32+0xa0+4,1);Reject([&]{resources::WorldEffectData::Decode(duplicate);});
    nlDefaultSeed=123;
    {
        WorldEffects unavailable(data);const auto before=unavailable.Snapshot();Check(before[0].id==3&&before[2].id==1,"Original AddStart order differs");
        Check(before[2].elapsed==2&&before[2].previous==2,"Original initialization counters differ");
        Check(unavailable.Trigger(95)==0&&unavailable.Seed()==123,"Absent original trigger changed RNG");
        Reject([&]{unavailable.Update(.25f);});Check(unavailable.Snapshot()[2].elapsed==2&&unavailable.Seed()==123,"Missing service advanced counters or RNG");
        std::thread wrong([&]{Reject([&]{unavailable.Trigger(95);});});wrong.join();unavailable.Release();
    }
    auto service=std::make_shared<Recording>();
    {
        WorldEffects value(data,service);service->world=&value;Check(value.Seed()==123,"Construction changed global RNG");
        Check(value.Update(0)==0&&value.Seed()==123,"Zero delta consumed RNG");
        Check(value.Update(.25f)==1&&service->order==std::vector<std::uint32_t>{1},"Original periodic first emission differs");
        Check(value.Seed()==Step(123),"Original world RNG recurrence differs");
        auto state=value.Snapshot();Check(state[2].elapsed==0&&state[2].previous==2.25f&&state[2].remaining==-1,"Original emission bookkeeping differs");
        Check(value.Trigger(39)==1,"Authored trigger not found");value.Update(.25f);state=value.Snapshot();
        Check(service->order==std::vector<std::uint32_t>({1,2})&&state[1].remaining==1&&state[1].previous==2&&state[1].elapsed==0,"Triggered emission countdown differs");
        Check(value.Update(.25f)==0&&value.Snapshot()[1].elapsed==0,"Nonzero timing advanced without a trigger");
        value.Reset();state=value.Snapshot();Check(state[2].elapsed==2&&state[2].previous==0&&state[1].remaining==2,"Reset/initialize distinction lost");
        value.SetActive(1,false);value.Trigger(7);nlDefaultSeed=99;
        Check(value.Update(.25f)==0&&value.Seed()==Step(99),"Failed probability consumed wrong RNG");
        Check(value.Snapshot()[0].elapsed==2&&value.Snapshot()[0].remaining==1,"Failed probability changed trigger counters");
        nlDefaultSeed=1;Check(value.Update(.25f)==1&&value.Snapshot()[0].remaining==0,"Probability retry did not emit");
        value.SetActive(1,true);value.Reset();value.Trigger(39);value.Trigger(7);nlDefaultSeed=1;service->order.clear();value.Update(.25f);
        Check(service->order.front()==3&&service->order.back()==1,"Original reverse world effect iteration order differs");
        const auto random=value.Seed();Reject([&]{value.Update(-1);});Reject([&]{value.Update(NAN);});Check(value.Seed()==random,"Invalid delta consumed RNG");
        auto retained=service->callbacks.front();value.SetView(Planes(),false);Check(!retained().rendering_enabled,"Retained callback lost current visibility state");
        service->fail_release=true;Reject([&]{value.Release();});Check(value.Snapshot().size()==3,"Failed release retired source state");
        service->fail_release=false;value.Release();Reject([&]{retained();});service->world=nullptr;Check(nlDefaultSeed==random,"Release rewound shared global RNG");
    }
    {
        auto failure=std::make_shared<Recording>();WorldEffects value(data,failure);failure->world=&value;failure->fail=true;nlDefaultSeed=17;
        Reject([&]{value.Update(.25f);});const auto state=value.Snapshot();
        Check(value.Failed()&&value.Seed()==Step(17)&&state[2].elapsed==2.25f&&state[2].previous==2&&state[2].remaining==-1,"Original pre-Emit side effects were lost or success counters fabricated");
        Reject([&]{value.Reset();});value.Release();failure->world=nullptr;
    }
}
EffectsRegistry::Handle Registry(const std::filesystem::path& folder,bool owned)
{
    auto files=std::make_shared<ParticleFiles>();const std::array<std::string,4> names=owned?std::array<std::string,4>{"Art/effects/effects.bun","Art/effects/effectsnonres.bun","Art/objects/effectsgeometry.bun","Art/objects/effectsgeometrytextures.rlt"}:std::array<std::string,4>{"multi.bun","nonresident.bun","geometry.bun","textures.rlt"};
    for(unsigned i=0;i<4;++i){files->data[i]=Read(folder/names[i]);files->source_sizes[i]=files->data[i].size();}return EffectsRegistry::FromFiles(files);
}
void Real(resources::WorldEffectData::Handle data,EffectsRegistry::Handle registry,bool owned)
{
    const auto before1=StandardAllocator.TotalFreeMemory(),before2=VirtualAllocator.TotalFreeMemory();const auto effects_seed=gEffectsRandomSeed;
    auto controllers=std::make_shared<ParticleControllers>(ParticleControllersOptions{256,32,0x9184eb0c});
    auto service=std::make_shared<WorldBillboardEffects>(registry,controllers);
    WorldEffects world(data,service);const auto global=nlDefaultSeed;
    Check(world.Trigger(95)==0&&world.Seed()==global,"Actual world Trigger95 changed state/RNG");
    const auto target=owned?0xbd2633c9U:1U;
    for(const auto& e:data->Records())world.SetActive(e.id,e.id==target);
    world.SetView(Planes(),true);Check(world.Trigger(39)==1,"Real source timing39 missing");
    Check(world.Update(1.f/60)==1,"Real world effect failed to create controller");
    auto visible=controllers->Snapshot();Check(visible.size()==1&&visible[0].id==1&&visible[0].frame.visible,"Real original controller initial state differs");
    const auto token=visible[0].token;unsigned samples=0,peak=0;
    for(unsigned frame=0;frame<120;++frame)
    {
        world.Update(1.f/60);controllers->Advance(1.f/60);auto current=controllers->Snapshot();
        for(const auto& c:current){samples+=c.particles;peak=std::max(peak,c.particles);}
        if(frame==2){world.SetView(Planes(),false);controllers->Advance(0);Check(!controllers->Snapshot().front().frame.visible,"World render gate did not reach original callback");world.SetView(Planes(),true);}
    }
    Check(samples>0&&peak>0,"Real source world effect emitted no particles");
    if(!owned)
    {
        const auto current=controllers->Snapshot();Check(!current.empty(),"Failure fixture controller ended early");
        controllers->SetCallbacks(current.front().token,[](auto&){throw std::runtime_error("Controlled live controller failure");},{});
        Reject([&]{controllers->Advance(0);});Check(controllers->Failed(),"Controlled update did not poison controller owner");
    }
    world.Release();if(!owned)controllers->Reset(123);Check(controllers->Snapshot().empty(),"World release retained controller callbacks/particles");service.reset();controllers->Release();controllers.reset();
    Check(StandardAllocator.TotalFreeMemory()==before1&&VirtualAllocator.TotalFreeMemory()==before2&&gEffectsRandomSeed==effects_seed,"World effects leaked original pools/RNG");
    std::cout<<(owned?"Owned":"Generated")<<" world effect39: "<<samples<<" live-particle samples, peak "<<peak<<"; actual world trigger95 has zero matches\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        const bool owned=argc==4&&std::string_view(argv[1])=="--owned";Check(owned||argc==2,"Use world_effects_tests FIXTURES or --owned WORLD_RES EFFECTS_FOLDER");
        std::vector<std::uint64_t> one(1<<20),two(1<<20);ResetStartupMemory();StandardAllocator.Initialize(one.data(),one.size()*8);VirtualAllocator.Initialize(two.data(),two.size()*8);gMemoryInitialized=1;
        const auto original=nlDefaultSeed;
        for(unsigned repeat=0;repeat<3;++repeat)
        {
            if(owned)
            {
                auto data=resources::WorldEffectData::Decode(Read(argv[2]));Check(data->Records().size()==88,"Owned world effect count differs");
                std::cout<<"world-effects-fingerprint="<<std::hex<<Fingerprint(*data)<<std::dec<<'\n';
                for(const auto& e:data->Records()){Check(e.timing!=95&&!e.animated,"Owned trigger/binding audit differs");Check(e.creation_flags==1&&e.repeat_offset==-1,"Owned source metadata differs");}
                Real(data,Registry(argv[3],true),true);
            }
            else{Generated(argv[1]);Real(resources::WorldEffectData::Decode(Read(std::filesystem::path(argv[1])/"single.res")),Registry(argv[1],false),false);}
        }
        nlDefaultSeed=original;ResetStartupMemory();std::cout<<checks<<" world-effect checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
