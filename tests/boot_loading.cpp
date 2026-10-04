#include "runtime/boot_loading.h"
#include "runtime/startup.h"
#include "runtime/graphics_memory.h"
#include "platform/path.h"
#include "Game/AsyncLoadingShared.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/gl/glMemoryInit.h"
#include <aurora/aurora.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <tuple>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>

extern "C" std::uint32_t ChargedGetBusClock();
using namespace mscharged;
namespace
{
using Blob = std::vector<std::uint8_t>;
unsigned checks = 0;
void Check(bool good, const char* message) { ++checks; if (!good) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Invalid boot operation accepted"); }
std::uint16_t Op(unsigned op, unsigned arg = 0) { return (op << 11) | arg; }
Blob Fixture(std::initializer_list<std::uint16_t> code, unsigned hash = 0xb53474ff, unsigned arguments = 0, unsigned flags = 0)
{
    Blob b;
    auto word = [&](std::uint32_t v) { for (int n : {24,16,8,0}) b.push_back(v >> n); };
    auto half = [&](unsigned v) { b.push_back(v >> 8); b.push_back(v); };
    for (unsigned v : {0xe11c2112u,1u,0u,0u,0u,unsigned(code.size()*2),7u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u}) word(v);
    word(hash); word(0); half(2); b.push_back(arguments); b.push_back(flags);
    for (auto v : code) half(v);
    for (auto c : std::array<std::uint8_t,7>{'m','a','r','k','e','r',0}) b.push_back(c);
    return b;
}
struct Arenas
{
    std::vector<std::uint64_t> mem1, mem2;
    Arenas(unsigned virtual_bytes = 8*1024*1024) : mem1(4*1024*1024/8), mem2(virtual_bytes/8)
    {
        ResetStartupMemory(); StandardAllocator.Initialize(mem1.data(), mem1.size()*8);
        VirtualAllocator.Initialize(mem2.data(), mem2.size()*8); gMemoryInitialized = 1;
        const GLMemoryRequirement requirements[] = {{GLM_Header,4096},{GLM_TextureData,4096}};
        const GLMemoryConfig config{4096,4096,requirements,2,4}; glInitMemory(&config);
    }
    ~Arenas() { glShutdownMemory(); ResetStartupMemory(); }
};
struct HostSession
{
    bool live = false;
    ~HostSession() { if (live) { ResetStartupMemory(); aurora_shutdown(); } }
    void Begin(int argc, char** argv)
    {
        const char* base = SDL_GetBasePath();
        Check(base != nullptr,"Cannot locate boot loading test data directory");
        const std::string path = std::string(base) + "boot-loading-test-data";
        std::filesystem::create_directories(PathFromUtf8(path));
        AuroraConfig config{};
        config.appName = "Charged boot loading check";
        config.userPath = config.cachePath = path.c_str(); config.resourcesPath = base;
        config.desiredBackend = BACKEND_NULL; config.logLevel = LOG_WARNING;
        config.windowWidth = 320; config.windowHeight = 240;
        config.windowPosX = config.windowPosY = -1;
        config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64*1024*1024;
        const auto host = aurora_initialize(argc,argv,&config); live = true;
        Check(host.window != nullptr,"Aurora initialization failed");
        InitializeStartupOS(); // Supplies the real bus clock used by original nlTicker.
        Check(ChargedGetBusClock() != 0,"Original ticker bus clock was not initialized");
    }
};
struct Memory
{
    unsigned mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    GLResourcePool* pool = glGetCurrentResourcePool();
    unsigned count = 0;
    Memory()
    {
        if (auto* head=glGetResourcePools())
        {
            auto* p=head;
            do { if (!p || ++count>128) throw std::runtime_error("Invalid resource pool ring"); p=p->m_next; } while(p!=head);
        }
    }
    void CheckSame() const
    {
        Memory current;
        Check(mem1==current.mem1 && mem2==current.mem2 && pool==current.pool && count==current.count,
            "Boot loading did not restore both arenas, pool list and current pool");
    }
};
void SharedRules()
{
    struct Stage
    {
        InterpreterFlow flow = InterpreterFlow::Continue;
        void StopWithoutUndo() { flow = InterpreterFlow::Pause; }
        void StopWithUndo() { flow = InterpreterFlow::Retry; }
    } stage;
    for (auto [finished,time,expected] : {
        std::tuple{true,44.f,InterpreterFlow::Continue}, {true,45.f,InterpreterFlow::Continue},
        {true,45.01f,InterpreterFlow::Pause}, {false,0.f,InterpreterFlow::Retry}, {false,99.f,InterpreterFlow::Retry}})
    {
        stage.flow=InterpreterFlow::Continue; FinishAsyncLoadingStepOrUndo(&stage,finished,time,45.f);
        Check(stage.flow==expected,"Original stage readiness/yield comparison changed");
    }
    // A generated readiness service exercises the actual original VM and shared
    // helper. It is a test input, never registered as a production game service.
    NativeInterpreter vm(Fixture({Op(2,73),Op(8,0),Op(8,1),Op(10)}));
    unsigned tries=0, after=0;
    vm.Bind({0,{InterpreterValueKind::Word},false,[&](auto args){
        Check(std::get<std::uint32_t>(args[0])==73,"Retry consumed or changed original argument");
        stage.flow=InterpreterFlow::Continue;
        FinishAsyncLoadingStepOrUndo(&stage,++tries==3,0,45);
        return InterpreterHostResult{{},stage.flow};
    }});
    vm.Bind({1,{},false,[&](auto){++after;return InterpreterHostResult{};}});
    vm.Execute(0xb53474ff);
    Check(vm.Status()==InterpreterStatus::Paused && tries==1 && after==0,"False readiness advanced the script");
    vm.Resume(); Check(tries==2 && after==0,"Retry skipped its original service");
    vm.Resume(); Check(vm.Status()==InterpreterStatus::Ready && tries==3 && after==1,"True readiness did not continue once");
    NativeInterpreter pause(Fixture({Op(8,0),Op(8,1),Op(10)}));
    unsigned first=0, second=0;
    pause.Bind({0,{},false,[&](auto){++first;stage.flow=InterpreterFlow::Continue;FinishAsyncLoadingStep(&stage,46,45);return InterpreterHostResult{{},stage.flow};}});
    pause.Bind({1,{},false,[&](auto){++second;return InterpreterHostResult{};}});
    pause.Execute(0xb53474ff); Check(first==1 && second==0,"Original stage yield failed");
    pause.Resume(); Check(first==1 && second==1,"Original successful yield repeated side effects");
}
void EntryAndLifetime()
{
    const auto minimal = Fixture({Op(10)});
    Reject([&]{BootLoading absent(Fixture({Op(10)},0));});
    Reject([&]{BootLoading args(Fixture({Op(10)},0xb53474ff,1));});
    Reject([&]{BootLoading result(Fixture({Op(10)},0xb53474ff,0,1));});
    for (std::size_t size=0;size<minimal.size();++size)
        Reject([&]{BootLoading truncated(resources::Bytes(minimal).first(size));});
    BootLoading no_graphics(minimal); Reject([&]{no_graphics.Begin();});
    Check(no_graphics.State()==BootLoadingState::Idle,"Rejected Begin changed boot state");
    no_graphics.Cancel(); Check(no_graphics.State()==BootLoadingState::Cancelled,"Idle cancel failed");
    no_graphics.Reset();
    for (unsigned run=0;run<3;++run)
    {
        Arenas arenas; Memory baseline;
        {
            auto bytes=Fixture({Op(3),Op(8,120),Op(8,4),Op(8,119),Op(8,61),Op(8,48),Op(8,83),Op(10)});
            BootLoading boot(bytes); bytes.clear();bytes.shrink_to_fit();
            Check(boot.Update()==BootLoadingState::Idle,"Idle Update started loading");
            boot.Begin(); Reject([&]{boot.Begin();});
            Check(boot.Update()==BootLoadingState::Running,"BEGIN falsely published frontend readiness");
            Check(boot.PersistentPool() && boot.PersistentPool()!=baseline.pool && glGetCurrentResourcePool()==baseline.pool,
                "Original persistent pool creation changed the active pool or failed");
            Check(boot.PersistentPool()->m_inventory && boot.Calls().size()==6,"Persistent pool inventory/service counts differ");
            Check(StandardAllocator.TotalFreeMemory()<baseline.mem1 && VirtualAllocator.TotalFreeMemory()<baseline.mem2,
                "Persistent pool did not allocate real storage in both arenas");
            Check(boot.Update()==BootLoadingState::Complete && boot.Update()==BootLoadingState::Complete,
                "Original RUN did not publish completion exactly once");
            bool wrong_thread=false;
            std::thread other([&]{try{boot.Reset();}catch(const std::logic_error&){wrong_thread=true;}});other.join();
            Check(wrong_thread && boot.State()==BootLoadingState::Complete,"Wrong-thread mutation was accepted");
            boot.Cancel();boot.Cancel();baseline.CheckSame();
            boot.Reset();boot.Begin();boot.Cancel();baseline.CheckSame();
            boot.Reset();boot.Begin();boot.Update(); // Destructor releases a live retained pool.
        }
        baseline.CheckSame();
        {
            BootLoading blocked(Fixture({Op(3),Op(8,120),Op(8,4),Op(8,41),Op(10)}));
            blocked.Begin();Check(blocked.Update()==BootLoadingState::Blocked,"Unprovided particle service was accepted");
            const std::array<unsigned,3> expected{120,4,41};
            Check(blocked.Stop() && blocked.Stop()->service==41 && std::ranges::equal(blocked.Calls(),expected),
                "Blocked service identity/order changed");
            Check(blocked.PersistentPool() && blocked.HostCalls()==3 && blocked.Instructions()==4,
                "Blocked execution lost its retained pool or execution counters");
            const auto error=blocked.Error();
            Check(blocked.Update()==BootLoadingState::Blocked && blocked.Error()==error && blocked.HostCalls()==3,
                "Terminal blocked polling reran side effects");
            Reject([&]{blocked.Begin();});
            blocked.Reset();baseline.CheckSame();blocked.Begin();blocked.Update();
        }
        baseline.CheckSame();
    }
}

void ErrorsAndAllocation()
{
    Arenas arenas; Memory baseline;
    for (auto bytes : {Fixture({Op(8,4),Op(8,4),Op(10)}),
                       Fixture({Op(8,4),Op(2,1),Op(8,120),Op(10)}),
                       Fixture({Op(8,4),Op(7,0)})})
    {
        BootLoading boot(bytes,InterpreterLimits{100,8,30,20});boot.Begin();
        Check(boot.Update()==BootLoadingState::Failed && !boot.Stop() && !boot.Error().empty(),
            "Duplicate pool, malformed argument or instruction budget was accepted");
        Check(boot.PersistentPool()!=nullptr,"Failed execution lost owned pool before explicit cleanup");
        boot.Cancel();baseline.CheckSame();
    }
    {
        BootLoading boot(Fixture({Op(8,11),Op(10)}));boot.Begin();
        Check(boot.Update()==BootLoadingState::Blocked && boot.Stop()->service==11 && !boot.PersistentPool(),
            "Font file subset was mistaken for original FontManager readiness");
    }
    baseline.CheckSame();
    {
        BootLoading boot(Fixture({Op(8,4),Op(10)}));
        const auto previous=g_fYieldScriptBlockingTimeMS;g_fYieldScriptBlockingTimeMS=std::numeric_limits<float>::quiet_NaN();
        Reject([&]{boot.Begin();});g_fYieldScriptBlockingTimeMS=previous;
        Check(boot.State()==BootLoadingState::Idle,"Invalid clock policy changed state");
        alignas(32) std::array<unsigned char,256> storage{}; MemoryAllocator empty{};
        empty.Initialize(storage.data(),storage.size());
        void* held=empty.Allocate(storage.size()-alignof(FreeBlockList),8,false);
        boot.Begin();
        { ScopedGameAllocator allocator(empty);
          Check(boot.Update()==BootLoadingState::Failed,"Native allocation failure was hidden"); }
        empty.Free(held);boot.Cancel();baseline.CheckSame();
    }
}
void PausedSessions()
{
    Arenas arenas;Memory baseline;
    std::uint32_t tick=0;
    const auto advance=ChargedGetBusClock()/4/20; // 50ms, original ticker units.
    auto clock=[&]{tick+=advance;return tick;};
    BootLoading boot(Fixture({Op(8,4),Op(8,89),Op(8,48),Op(8,89),Op(10)}),{},clock);
    boot.Begin();Check(boot.Update()==BootLoadingState::Running && boot.Calls().size()==2
        && boot.Instructions()==2 && boot.HostCalls()==2,"First stage yield did not retain its execution state");
    Check(boot.Update()==BootLoadingState::Running && boot.Calls().size()==4
        && boot.Instructions()==4 && boot.HostCalls()==4,"Resumed stage counters were reset or repeated");
    Check(boot.Update()==BootLoadingState::Complete && boot.Instructions()==5 && boot.HostCalls()==4,
        "Resumed return did not complete the original sequence");
    boot.Update();Check(boot.Instructions()==5 && boot.HostCalls()==4,"Terminal polling double-counted dispatch");
    boot.Cancel();baseline.CheckSame();
    BootLoading loop(Fixture({Op(8,4),Op(8,89),Op(7,1)}),InterpreterLimits{100,8,100,3},clock);
    loop.Begin();Check(loop.Update()==BootLoadingState::Running,"Loop did not yield");
    Check(loop.Update()==BootLoadingState::Running,"Second loop yield failed prematurely");
    Check(loop.Update()==BootLoadingState::Failed && loop.Calls().size()==3 && loop.HostCalls()==4,
        "Session trace limit failed to bound repeated paused dispatches");
    loop.Cancel();baseline.CheckSame();
    BootLoading* owner=nullptr;bool rejected=false;
    BootLoading reentry(Fixture({Op(8,48),Op(10)}),{},[&]{
        try{owner->Reset();}catch(const std::logic_error&){rejected=true;}
        return clock();
    });owner=&reentry;
    reentry.Begin();Check(reentry.Update()==BootLoadingState::Running && rejected,"Clock callback reentry was accepted");
    reentry.Cancel();baseline.CheckSame();
    BootLoading bad_clock(Fixture({Op(8,4),Op(10)}),{},[]()->std::uint32_t{throw std::runtime_error("clock failed");});
    bad_clock.Begin();Check(bad_clock.Update()==BootLoadingState::Failed && bad_clock.Instructions()==0,
        "Clock exception advanced bytecode");bad_clock.Cancel();baseline.CheckSame();
}
void TextureArenaFailure()
{
    Arenas arenas(1024*1024);Memory baseline;
    BootLoading boot(Fixture({Op(8,4),Op(10)}));boot.Begin();
    Check(boot.Update()==BootLoadingState::Failed && !boot.PersistentPool(),"Oversized original texture pool succeeded");
    baseline.CheckSame();boot.Reset();baseline.CheckSame();
}
void Owned(const char* path)
{
    std::ifstream input(path,std::ios::binary);Check(bool(input),"Cannot open owned boot script");
    Blob bytes((std::istreambuf_iterator<char>(input)),{});
    const auto script=resources::ReadScriptBytecode(bytes);
    Check(bytes.size()==2070 && script->functions.size()==17 && script->code.size()==498,"Owned boot script profile changed");
    Arenas arenas;Memory baseline;
    {
        BootLoading boot(bytes);bytes.clear();bytes.shrink_to_fit();boot.Begin();
        Check(boot.Update()==BootLoadingState::Blocked && boot.Stop()->service==41,"Owned script did not stop at actual particle loading");
        const std::array<unsigned,3> expected{120,4,41};
        Check(std::ranges::equal(boot.Calls(),expected) && boot.HostCalls()==3 && boot.Instructions()==5,
            "Owned original call/stack sequence differs from independent disassembly");
        Check(boot.PersistentPool() && glGetCurrentResourcePool()==baseline.pool,"Owned prefix did not allocate the original persistent pool");
        std::cout<<"Owned BootLoadingToFE: 17 functions, 498 instructions; executed "<<boot.Instructions()
                 <<" instructions, "<<boot.HostCalls()<<" host calls; blocked "<<boot.Stop()->service<<": "<<boot.Error()<<'\n';
    }
    baseline.CheckSame();
}
}
int main(int argc,char** argv)
{
    try
    {
        HostSession host; host.Begin(argc,argv);
        Check(gPersistentResourceRequirements.entries[0].mType==GLM_Header
            && gPersistentResourceRequirements.entries[0].mSize==0x3c00
            && gPersistentResourceRequirements.entries[1].mType==GLM_TextureData
            && gPersistentResourceRequirements.entries[1].mSize==0x390800,"Original persistent requirements changed");
        SharedRules();EntryAndLifetime();ErrorsAndAllocation();PausedSessions();TextureArenaFailure();
        for(int i=1;i<argc;++i)Owned(argv[i]);
        std::cout<<checks<<" boot loading checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
