#include "runtime/particle_simulation.h"
#include "runtime/graphics_memory.h"
#include "runtime/startup.h"
#include "Game/Effects/ParticleSystem.h"
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F action, std::source_location at = std::source_location::current())
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid particle operation accepted at line " + std::to_string(at.line())); }
void Near(float a, float b) { Check(std::isfinite(a) && std::abs(a-b) < .00002f, "Original particle numerical result differs"); }
std::vector<std::uint8_t> Read(const std::filesystem::path& path)
{ std::ifstream f(path,std::ios::binary); if (!f) throw std::runtime_error("Cannot open particle input"); return {std::istreambuf_iterator<char>(f),{}}; }
struct Arenas
{
    void* mem1 = ::operator new(4*1024*1024, std::align_val_t(64));
    void* mem2 = ::operator new(4*1024*1024, std::align_val_t(64));
    Arenas() { ResetStartupMemory(); StandardAllocator.Initialize(mem1,4*1024*1024); VirtualAllocator.Initialize(mem2,4*1024*1024); gMemoryInitialized=1; }
    ~Arenas() { ResetStartupMemory(); ::operator delete(mem1,std::align_val_t(64)); ::operator delete(mem2,std::align_val_t(64)); }
};
EffectsRegistry::Handle Load(const std::filesystem::path& folder, const std::string& mode, bool owned=false)
{
    auto files = std::make_shared<ParticleFiles>();
    const std::array<std::string,4> names = owned ? std::array<std::string,4>{"Art/effects/effects.bun","Art/effects/effectsnonres.bun","Art/objects/effectsgeometry.bun","Art/objects/effectsgeometrytextures.rlt"}
        : std::array<std::string,4>{mode+".bun","nonresident.bun","geometry.bun","textures.rlt"};
    for (unsigned i=0;i<4;++i) { files->data[i]=Read(folder/names[i]); files->source_sizes[i]=files->data[i].size(); }
    return EffectsRegistry::FromFiles(files);
}
std::uint32_t Random(std::uint32_t seed, unsigned count)
{
    for (unsigned i=0;i<count;++i)
    {
        // Independently expressed unsigned bit recurrence; even zero-width
        // randomized ranges consume both original RNG calls.
        const std::uint32_t x = seed ^ 0x1d872b41U;
        const std::uint32_t shifted = x ^ (x >> 5);
        seed = shifted ^ x ^ (shifted << 27);
    }
    return seed;
}
void Basic(const std::filesystem::path& folder)
{
    const auto before1=StandardAllocator.TotalFreeMemory(), before2=VirtualAllocator.TotalFreeMemory();
    auto registry=Load(folder,"basic"); const auto saved=uSeed;
    {
        ParticleSimulation simulation(registry,0x81f2a311,0,{32,123});
        registry.reset(); Check(simulation.Texture()!=nullptr,"Simulation did not retain its texture");
        Reject([&] { ParticleSimulation other(Load(folder,"basic"),0x81f2a311,0); });
        Reject([&] { simulation.Advance(-1); }); Reject([&] { simulation.Advance(INFINITY); });
        Reject([&] { simulation.Advance(std::numeric_limits<float>::quiet_NaN()); });
        std::thread wrong([&] { Reject([&] { simulation.Advance(0); }); Reject([&] { simulation.Release(); }); }); wrong.join();
        unsigned draws=0;
        for (unsigned step=0;step<8;++step)
        {
            simulation.Advance(.25f); const auto particles=simulation.Snapshot(); const auto quads=simulation.Sample();
            const unsigned count=step<3?2*(step+1):step<7?6:4;
            Check(particles.size()==count && quads.size()==count,"Original emission/removal count differs"); draws+=quads.size();
            const unsigned random_calls=(std::min(step+1,7U)*2) + std::min(step+1,7U)*2*25;
            Check(simulation.Seed()==Random(123,random_calls),"Original RNG call order/count differs");
            for(unsigned i=0;i<particles.size();++i)
            {
                const auto& p=particles[i]; Near(p.lifespan,1); Near(p.direction[2],1); Near(p.initial_position[2],0);
                const unsigned updates=unsigned(p.elapsed*4);
                Near(p.position[2],2*p.elapsed + .0625f*updates*(updates+1)/2);
                const unsigned colour=unsigned(24*p.elapsed);
                Check(quads[i].colour==std::array<std::uint8_t,4>{std::uint8_t(colour*8),std::uint8_t(colour*4),std::uint8_t(colour*2),std::uint8_t(255-colour*8)},"Original colour sample differs");
                Near(quads[i].position[0][0],1); Near(quads[i].position[0][1],1); Near(quads[i].position[0][2],p.position[2]);
                const unsigned frame=updates%4; Near(quads[i].uv[0][0],(frame%2)*.5f+.5f); Near(quads[i].uv[0][1],(frame/2)*.5f);
            }
        }
        Check(draws==40,"Unexpected diagnostic draw count");
        simulation.Die(); Check(!simulation.Advance(1)&&simulation.Snapshot().empty(),"Original Die did not drain live particles");
        for(unsigned repeat=0;repeat<8;++repeat)
        { simulation.Reset(123); simulation.Advance(.25f); Check(simulation.Snapshot().size()==2 && simulation.Seed()==Random(123,52),"Reset changed source state"); }
        simulation.Release(); simulation.Release(); Check(!simulation.Active(),"Released simulation stayed active"); Reject([&]{simulation.Advance(0);});
    }
    Check(uSeed==saved&&ParticleSystem::m_NumInstances==0,"Particle globals leaked across session");
    Check(before1==StandardAllocator.TotalFreeMemory()&&before2==VirtualAllocator.TotalFreeMemory(),"Particle lists or atlas leaked arena memory");
}
void Profiles(const std::filesystem::path& folder)
{
    for(const char* mode:{"badlinger","model","light","event","frames","life","missing","pose"})
        Reject([&] { ParticleSimulation simulation(Load(folder,mode),0x81f2a311,0); });
    Reject([&] { ParticleSimulation simulation(Load(folder,"basic"),0x81f2a311,0,{0,0}); });
    Reject([&] { ParticleSimulation simulation(Load(folder,"basic"),0x81f2a311,1); });
    for(const char* mode:{"local","sphere","spindle","hemisphere","disc","curve","burst","delay","linger","atlas1","atlas9","atlas16","atlas25","atlas36"})
    {
        ParticleSimulation simulation(Load(folder,mode),0x81f2a311,0,{5,123});
        for(unsigned step=0;step<12;++step)
        {
            simulation.Advance(.125f); const auto particles=simulation.Snapshot(); const auto quads=simulation.Sample();
            Check(particles.size()<=5 && quads.size()==particles.size(),"Fixed particle pool overflowed");
            for(const auto& q:quads) for(const auto& uv:q.uv) for(float f:uv) Check(std::isfinite(f)&&f>=0&&f<1.000001f,"Atlas UV outside source tile bounds");
        }
        simulation.Die(); for(unsigned i=0;i<8;++i)simulation.Advance(.25f);
        Check(simulation.Snapshot().empty(),"Profile retained dead particles");
    }
    {
        ParticleSimulation delayed(Load(folder,"delay"),0x81f2a311,0);
        delayed.Advance(.25f); delayed.Advance(.25f); Check(delayed.Elapsed()==0 && delayed.Snapshot().empty(),"Original delay did not discard overshoot");
        delayed.Advance(.25f);Check(delayed.Snapshot().size()==2,"Delayed source emission did not resume");
    }
    {
        ParticleSimulation simulation(Load(folder,"linger"),0x81f2a311,0);
        simulation.Advance(.5f);Near(simulation.Elapsed(),.5f);simulation.Advance(.25f);Near(simulation.Elapsed(),.25f);
    }
    {
        ParticleSimulation burst(Load(folder,"burst"),0x81f2a311,0,{3,123});
        burst.Advance(0);Check(burst.Snapshot().size()==3 && burst.Seed()==Random(123,77),"Burst capacity changed RNG consumption");
        Check(!burst.Advance(1),"Nonlingering burst did not finish");
    }
    {
        ParticleSimulation simulation(Load(folder,"angle"),0x81f2a311,0);
        simulation.Advance(1);simulation.Sample();simulation.Advance(1);simulation.Sample();simulation.Advance(1);
        Reject([&]{simulation.Sample();});Check(simulation.Failed(),"Unsafe angle did not poison the session");
        Reject([&]{simulation.Advance(0);});Reject([&]{simulation.Snapshot();});
        simulation.Reset(17);Check(!simulation.Failed()&&simulation.Snapshot().empty(),"Reset did not recover failed simulation");
    }
    {
        ParticleSimulation simulation(Load(folder,"flip"),0x81f2a311,0);
        simulation.Advance(.25f);const auto p=simulation.Snapshot();const auto q=simulation.Sample();
        Check(p.size()==2 && p[0].flip_uv && p[1].flip_uv,"Original UV flip threshold failed");
        Near(q[0].uv[0][0],.5f);Near(q[0].uv[1][0],1.f);
    }
    {
        ParticleSimulation simulation(Load(folder,"curve"),0x81f2a311,0);
        simulation.Advance(.25f);auto q=simulation.Sample();Near(q.back().position[0][0],1.25f);
        simulation.Advance(.25f);q=simulation.Sample();Near(q.back().position[0][0],2.5f);
    }
    // Call the selected original guard directly to cover terminal colour and
    // unsafe casts without manufacturing unreachable published live particles.
    auto registry=Load(folder,"basic");auto group=registry->FindGroup(0x81f2a311);
    ParticleSimulation guard_session(registry,0x81f2a311,0);
    nlDLListSlotPool<Particle*> free;
    ParticleSystem original(ParticleSystem::NativeSimulation{},group->m_specs[0].m_pTemplate,&free,&group->m_specs[0],1);
    Particle p{};p.pTemplate=group->m_specs[0].m_pTemplate;p.lifeSpan=1;p.timeElapsed=1;p.size=2;p.sizeScale=1;
    ParticleReturn result{}; const nlVector3 r{1,0,0},u{0,1,0};
    Reject([&] {original.UpdateParticle(&result,&p,p.pTemplate,r,u,nullptr);});
    p.timeElapsed=0;p.rot=32768; Reject([&] {original.UpdateParticle(&result,&p,p.pTemplate,r,u,nullptr);});
    p.rot=0;p.FPS=INFINITY;Reject([&] {original.UpdateParticle(&result,&p,p.pTemplate,r,u,nullptr);});
    p.FPS=0;p.timeElapsed=.0625f;
    original.UpdateParticle(&result,&p,p.pTemplate,r,u,nullptr);
    Check(result.c.c[0]==12&&result.c.c[1]==6&&result.c.c[2]==3&&result.c.c[3]==243,"Fractional colour interpolation differs");
    p.timeElapsed=std::nextafter(1.f,0.f);original.UpdateParticle(&result,&p,p.pTemplate,r,u,nullptr);
    Check(result.c.c[0]<=192,"Last representable live colour escaped authored channel");
    guard_session.Release();p.timeElapsed=0;
    Reject([&] {original.UpdateParticle(&result,&p,p.pTemplate,r,u,nullptr);}); // no atlas outside owner
}
void Failures(const std::filesystem::path& folder)
{
    const auto before=VirtualAllocator.TotalFreeMemory();
    for(unsigned bytes:{64,256,1024,4096})
    {
        alignas(64) unsigned char storage[8192];MemoryAllocator small{};small.Initialize(storage,bytes);
        // Atlas allocations follow the owner's explicit MEM2 arena; temporarily
        // install the real tiny allocator to exercise every partial allocation.
        const auto saved=VirtualAllocator;VirtualAllocator=small;
        Reject([&] {ParticleSimulation simulation(Load(folder,"basic"),0x81f2a311,0,{4096,0});});
        Check(VirtualAllocator.TotalFreeMemory()==small.TotalFreeMemory(),"Failed particle acquisition leaked allocations");
        VirtualAllocator=saved;
    }
    Check(VirtualAllocator.TotalFreeMemory()==before,"Arena rollback changed original allocator");
    ParticleSimulation simulation(Load(folder,"basic"),0x81f2a311,0);
    simulation.Advance(.125f);simulation.Release();
}
void Owned(const std::filesystem::path& folder, std::uint32_t group, unsigned spec)
{
    auto registry=Load(folder,"",true);const auto before=VirtualAllocator.TotalFreeMemory();
    unsigned peak=0,quads=0;std::uint32_t seed=0;
    {
        ParticleSimulation simulation(registry,group,spec);
        for(unsigned i=0;i<120;++i){simulation.Advance(1.f/60);auto q=simulation.Sample();peak=std::max(peak,unsigned(q.size()));quads+=q.size();}
        simulation.Die();unsigned frames=0;
        while(simulation.Advance(1.f/60)&&++frames<3600)quads+=simulation.Sample().size();
        Check(peak>0 && quads>0,"Owned profile did not emit any particles/quads");
        Check(simulation.Snapshot().empty(),"Owned particles did not drain");seed=simulation.Seed();
    }
    Check(VirtualAllocator.TotalFreeMemory()==before,"Owned particle memory leaked");
    std::cout<<"Owned group="<<std::hex<<group<<std::dec<<" spec="<<spec<<" peak="<<peak<<" quads="<<quads<<" final_seed="<<seed<<'\n';
}
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==2||argc==5,"Supply fixture folder or owned folder --owned hash spec");
        Arenas arenas;
        if(argc==5){Check(std::string_view(argv[2])=="--owned","Unknown mode");Owned(argv[1],std::stoul(argv[3],nullptr,16),std::stoul(argv[4]));}
        else {Basic(argv[1]);Profiles(argv[1]);Failures(argv[1]);}
        std::cout<<checks<<" particle simulation checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
