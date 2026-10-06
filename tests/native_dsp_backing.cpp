#include "platform/dsp_memory.h"
#include "platform/dsp_memory_abi.h"
#include "platform/dsp_mailbox.h"
#include "platform/interrupt_controller.h"
#include "dsp_source_module.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <SDL3/SDL_loadso.h>
#include <dlfcn.h>
extern "C" {
#include <revolution/dsp.h>
}
void AuroraResetNativeAddresses();
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <string>
namespace {
using namespace mscharged::platform;
std::atomic<unsigned> checks{};
void Check(bool result,const char* message) {++checks;if (!result) throw std::runtime_error(message);}
template<class F> void Throws(F&& function,const char* message) {
    bool rejected=false;try {function();}catch(const std::exception&) {rejected=true;}Check(rejected,message);
}
struct SDKLifetime {
    bool live{};
    ~SDKLifetime() {if(live)aurora_shutdown();}
};
void SourceWireGate(NativeDSPMailboxEndpoint device,DSPTask* old,DSPTask* next,const std::array<u32,10>& expected) {
    std::exception_ptr failure;
    std::thread reader([&] {
        try {
            for (auto word:expected) {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
                u16 high;
                do {
                    high=DSPBackendMailToHigh(device);
                    if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("original task word never reached device wire");
                    std::this_thread::yield();
                } while((high&0x8000)==0);
                const u16 low=DSPBackendMailToLow(device);
                const u32 actual=static_cast<u32>(high)<<16|low;
                Check(actual==word,"actual original task mail order/value differs from authored field oracle");
            }
        }catch(...){failure=std::current_exception();}
    });
    __DSP_exec_task(old,next);
    reader.join();if(failure)std::rethrow_exception(failure);
    Check(!DSPCheckMailToDSP(),"source task returned without device acknowledging final word");
}
struct ModuleLease {
    std::string path;
    std::vector<SDL_SharedObject*> handles;
    static BOOL Retain(void* context) noexcept {
        auto& self=*static_cast<ModuleLease*>(context);
        auto* handle=SDL_LoadObject(self.path.c_str());
        if(!handle) return FALSE;
        try {self.handles.push_back(handle);}
        catch(...){SDL_UnloadObject(handle);return FALSE;}
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& self=*static_cast<ModuleLease*>(context);
        auto* handle=self.handles.back();self.handles.pop_back();SDL_UnloadObject(handle);
    }
};
OSNativeStaticMemoryOwner Owner(ModuleLease& lease,const char* module,const char* resource,void* address,u32 bytes,bool writable) {
    return {module,resource,address,bytes,writable?TRUE:FALSE,&lease,ModuleLease::Retain,ModuleLease::Release};
}
bool ImageLoaded(const char* path) {
    void* handle=dlopen(path,RTLD_NOW|RTLD_NOLOAD);
    if(handle){dlclose(handle);return true;}return false;
}
void Run(int argc,char** argv) {
    Check(argc==3,"fixture requires the actual compiled image path and SHA256 identity");
    Check(std::strlen(argv[2])==64,"fixture identity must be actual native image SHA256");
    Check(DSPCheckInit()==FALSE,"transport must not invent DSP initialization");
    SDKLifetime sdk;
    const auto directory=std::filesystem::absolute("sdk-data").string();
    std::filesystem::create_directories(directory);
    AuroraConfig config{};config.appName="Original DSP backing qualifier";
    config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
    config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;
    config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual SDK window missing");sdk.live=true;OSInit();
    Check(OSGetPhysicalMemSize()==0x01800000,"actual original MEM1 bank size changed");
    auto* mem1=static_cast<u8*>(OSPhysicalToCached(0));
    auto* mem2=static_cast<u8*>(OSGetMEM2ArenaLo());
    Check((reinterpret_cast<std::uintptr_t>(mem2)&31)==0,"real MEM2 DMA backing must align with its physical bank");
    Check(reinterpret_cast<std::uintptr_t>(mem2)>0xffffffffULL,"MEM2 fixture must exercise native address width");
    std::array<u8,512> zero{};
    Check(std::memcmp(mem2,zero.data(),zero.size())==0,"MEM2 alignment adaptation changed original zero initialization");
    Check(OSCachedToPhysical(mem2)==0x10000000 && OSPhysicalToCached(0x10000000)==mem2,
          "canonical MEM2 physical base/inverse differs from actual Wii bank");
    Check(OSPhysicalToUncached(0x10000000)==mem2 && OSUncachedToPhysical(mem2)==0x10000000,
          "actual cached/uncached inverse must agree");
    Throws([&]{OSPhysicalToCached(0x20000000);},"out-of-bank physical word succeeded");
    Throws([&]{OSPhysicalToCached(0xffffffff);},"overflow/status word became a physical alias");

    ModuleLease lease{std::filesystem::absolute(argv[1]).string(),{}};
    auto* initial=SDL_LoadObject(lease.path.c_str());Check(initial!=nullptr,"actual source leaf module could not load");
    auto function=reinterpret_cast<OriginalDSPData(*)()>(SDL_LoadFunction(initial,"OriginalDSPDataForFixture"));
    Check(function!=nullptr,"actual source descriptor export absent");
    const auto data=function();
    Check(data.code && data.code_bytes>0 && data.dram && data.dram_bytes==64,"original AX source data extents changed");
    Check(data.code!=data.dram,"distinct genuine source owners collapsed");
    Check(reinterpret_cast<std::uintptr_t>(data.code)>0xffffffffULL,"actual original module data must exercise native width");
    std::vector<u8> authored(static_cast<const u8*>(data.code),static_cast<const u8*>(data.code)+data.code_bytes);
    std::array<u8,64> original_dram{};
    Check(std::memcmp(data.dram,original_dram.data(),original_dram.size())==0,"original static AX DRAM initialization changed");
    auto code_owner=Owner(lease,argv[2],"axDspSlave",data.code,data.code_bytes,false);
    auto dram_owner=Owner(lease,argv[2],"__AXDramImage",data.dram,data.dram_bytes,true);
    Throws([&]{OSNativeRegisterStaticMemory(nullptr);},"null owner invented native backing");
    auto bad=code_owner;bad.retain=nullptr;
    Throws([&]{OSNativeRegisterStaticMemory(&bad);},"owner lacking a real lifetime lease succeeded");
    const auto pristine_high=OSGetArenaHi();
    bad=code_owner;bad.bytes=0xffffffff;
    Throws([&]{OSNativeRegisterStaticMemory(&bad);},"oversized physical reservation succeeded");
    Check(OSGetArenaHi()==pristine_high && lease.handles.empty(),"failed reserve mutated backing/retained fake ownership");
    const auto code=OSNativeRegisterStaticMemory(&code_owner);
    const auto dram=OSNativeRegisterStaticMemory(&dram_owner);
    const u32 rounded=(data.code_bytes+31)&~u32(31);
    const u32 expected_code=0x01800000-rounded,expected_dram=expected_code-64;
    Check(code.physical_address==expected_code && dram.physical_address==expected_dram,
          "actual reserved addresses differ from independent aligned arena-high oracle");
    Check((code.physical_address&31)==0 && (dram.physical_address&31)==0,"static physical reservations lost 32-byte alignment");
    Check(OSGetArenaHi()==mem1+expected_dram && OSGetArenaLo()==mem1+0x4000,
          "static physical reservations were not removed before original game backing capture");
    Check(OSPhysicalToCached(expected_code)==data.code && OSCachedToPhysical(data.code)==expected_code,
          "shared static bus pointer/inverse is incoherent");
    Check(OSPhysicalToCached(expected_dram+63)==static_cast<u8*>(data.dram)+63,
          "last retained static byte lost its exact native inverse");
    Throws([&]{OSCachedToPhysical(mem1+expected_code);},"reserved shadow created a second pointer alias");
    if(rounded!=data.code_bytes) Throws([&]{OSPhysicalToCached(expected_code+data.code_bytes);},"reservation padding invented native data");
    Throws([&]{OSNativeRegisterStaticMemory(&code_owner);},"duplicate owner identity succeeded");
    Throws([&]{AuroraResetNativeAddresses();},"SDK reset discarded live source module backing");
    Check(lease.handles.size()==2,"shared service did not retain actual module leases");
    SDL_UnloadObject(initial);
    Check(ImageLoaded(lease.path.c_str()),"source image unloaded before actual retained backings drained");

    const auto endpoint=AttachNativeDSPMEM1();
    Throws([&]{PinNativeDSPMEM1(data.code,data.code_bytes,false);},"explicit MEM1 pin silently gained static semantics");
    Throws([&]{PinNativeDSPMEM1(mem2,32,true);},"explicit MEM1 pin silently gained MEM2 semantics");
    auto code_pin=PinNativeDSPMemory(data.code,data.code_bytes,false);
    auto dram_pin=PinNativeDSPMemory(data.dram,64,true);
    Check(ChargedDSPTaskMemoryWord(data.code,data.code_bytes,0)==expected_code,"genuine AX DSP code word changed");
    Check(ChargedDSPTaskMemoryWord(data.dram,64,1)==expected_dram,"genuine AX context word changed");
    Throws([&]{OSNativeReleaseStaticMemory(code);},"live DSP pin failed to retain its source image backing");
    Throws([&]{PinNativeDSPMemory(data.code,1,true);},"read-only static owner allowed device writes");
    Throws([&]{PinNativeDSPMemory(data.code,static_cast<std::size_t>(data.code_bytes)+1,false);},"pin crossed exact original code extent");
    Throws([&]{PinNativeDSPMemory(static_cast<u8*>(data.dram)+64,1,true);},"unowned adjacent static bytes got a carrier");
    std::vector<u8> copy(data.code_bytes);
    DSPBackendReadMemory(endpoint,expected_code,copy.data(),copy.size());
    Check(copy==authored,"real device copy changed original DSP source bytes");
    Throws([&]{DSPBackendWriteMemory(endpoint,expected_code,zero.data(),1);},"device wrote source code through another alias");
    std::array<u8,64> context{};for(unsigned i=0;i<context.size();++i)context[i]=static_cast<u8>(i*7+3);
    DSPBackendWriteMemory(endpoint,expected_dram,context.data(),context.size());
    Check(std::memcmp(data.dram,context.data(),context.size())==0,"device output did not reach original static AX DRAM");

    auto* dma=static_cast<u8*>(OSAllocFromMEM2ArenaLo(512,32));
    Check(dma==mem2,"actual MEM2 source arena request altered the hardware base");
    for(unsigned i=0;i<zero.size();++i)zero[i]=static_cast<u8>(i*13+17);
    auto dma_pin=PinNativeDSPMemory(dma,512,true);
    Check(ChargedDSPTaskMemoryWord(dma,512,1)==0x10000000,"MEM2 task word was a token/truncation instead of physical bank offset");
    Check(ChargedDSPTaskMemoryWord(dma+511,1,0)==0x100001ff,"last pinned MEM2 byte rejected");
    Throws([&]{PinNativeDSPMemory(mem2+64u*1024u*1024u-1,2,true);},"MEM2 physical pin crossed its actual bank");
    Throws([&]{ChargedDSPTaskMemoryWord(dma,513,0);},"MEM2 source request crossed retained extent");
    std::exception_ptr failure;
    std::thread worker([&]{
        try {
            DSPBackendWriteMemory(endpoint,0x10000000,zero.data(),zero.size());
            std::array<u8,512> bytes{};DSPBackendReadMemory(endpoint,0x10000000,bytes.data(),bytes.size());
            Check(bytes==zero,"worker physical MEM2 read/write changed raw DMA bytes");
            std::vector<u8> code_bytes(data.code_bytes);DSPBackendReadMemory(endpoint,expected_code,code_bytes.data(),code_bytes.size());
            Check(code_bytes==authored,"worker static lookup lost retained module backing");
            Throws([&]{PinNativeDSPMemory(data.dram,1,true);},"device worker registered source ownership");
            Throws([&]{OSNativeReleaseStaticMemory(dram);},"device worker released the source module owner");
        }catch(...){failure=std::current_exception();}
    });worker.join();if(failure)std::rethrow_exception(failure);
    Check(std::memcmp(dma,zero.data(),zero.size())==0,"actual MEM2 backing differs from completed device bytes");

    InitializeNativeInterruptController();const auto wire=AttachNativeDSPMailboxes();
    DSPTask previous{},next{};
    previous.dramMmemAddr=data.dram;previous.dramMmemLen=64;previous.dramDspAddr=0x0cd2;
    next.iramMmemAddr=data.code;next.iramMmemLen=data.code_bytes;next.iramDspAddr=0;
    next.dramMmemAddr=data.dram;next.dramMmemLen=64;next.dramDspAddr=0x0cd2;
    next.startVector=data.initial_vector;next.resumeVector=data.resume_vector;next.state=DSP_TASK_STATE_0;
    std::array<u32,10> first{expected_dram,64,0x0cd2,expected_code,data.code_bytes,0,data.initial_vector,0,0,0};
    for(auto&word:first)word|=0x80000000;
    SourceWireGate(wire,&previous,&next,first);
    // Bounded original scheduler request with a genuine MEM2 restore span.
    // This does not assert that AX selected this span or that DSP firmware ran.
    next.state=DSP_TASK_STATE_2;next.dramMmemAddr=dma;next.dramMmemLen=512;
    std::array<u32,10> second{0,0,0,expected_code,data.code_bytes,0,data.resume_vector,0x10000000,512,0x0cd2};
    for(auto&word:second)word|=0x80000000;
    SourceWireGate(wire,nullptr,&next,second);
    Check(DSPCheckInit()==FALSE,"physical/memory transport fabricated a DSP_INIT completion");

    OSAllocFromArenaLo(32,32); // A genuine source-side capture makes new high reservations illegal.
    ReleaseNativeDSPMemory(dram_pin);OSNativeReleaseStaticMemory(dram);
    auto late=dram_owner;late.resource_identity="late-new-resource";
    const auto captured_high=OSGetArenaHi();const auto leases=lease.handles.size();
    Throws([&]{OSNativeRegisterStaticMemory(&late);},"post-capture arena bump overlapped already-owned game backing");
    Check(OSGetArenaHi()==captured_high && lease.handles.size()==leases,"late failed reservation mutated captured memory/owners");
    Throws([&]{OSPhysicalToCached(expected_dram);},"released static mapping exposed reserved shadow backing");
    Throws([&]{DSPBackendReadMemory(endpoint,expected_dram,context.data(),1);},"released source pin retained device access");
    const auto restored=OSNativeRegisterStaticMemory(&dram_owner);
    Check(restored.physical_address==dram.physical_address && restored.identity!=dram.identity,
          "persistent exact resource identity lost its reserved address/stale-handle distinction");
    Throws([&]{OSNativeReleaseStaticMemory(dram);},"old owner handle released newly retained static backing");
    OSNativeReleaseStaticMemory(restored);
    ReleaseNativeDSPMemory(code_pin);ReleaseNativeDSPMemory(dma_pin);
    Throws([&]{DSPBackendReadMemory(endpoint,0x10000000,zero.data(),1);},"released MEM2 pin retained device ownership");
    OSNativeReleaseStaticMemory(code);
    Check(lease.handles.empty() && !ImageLoaded(lease.path.c_str()),"actual source image did not unload after all backing/device owners drained");
    Throws([&]{OSPhysicalToCached(expected_code);},"unloaded module retained a physical inverse");
    Throws([&]{OSNativeReleaseStaticMemory(code);},"stale unloaded owner handle succeeded");
    // The same genuine image can reload into the persistent reserved range.
    initial=SDL_LoadObject(lease.path.c_str());Check(initial!=nullptr,"actual source image reload failed");
    function=reinterpret_cast<OriginalDSPData(*)()>(SDL_LoadFunction(initial,"OriginalDSPDataForFixture"));
    const auto reloaded=function();
    code_owner.address=reloaded.code;
    const auto reload=OSNativeRegisterStaticMemory(&code_owner);
    Check(reload.physical_address==expected_code && reload.identity!=code.identity,"same owner identity became a transient wire token");
    Check(std::memcmp(OSPhysicalToCached(expected_code),authored.data(),authored.size())==0,"original source code did not survive native image reload");
    SDL_UnloadObject(initial);OSNativeReleaseStaticMemory(reload);
    Check(lease.handles.empty() && !ImageLoaded(lease.path.c_str()),"reloaded actual native module lifetime did not drain");
    DetachNativeDSPMEM1();DetachNativeDSPMailboxes();ShutdownNativeInterruptController();
    aurora_shutdown();sdk.live=false;
    Check(OSGetArenaLo()==nullptr && OSGetMEM2ArenaLo()==nullptr,"actual SDK arenas did not release after physical owners drained");
    Check(DSPCheckInit()==FALSE,"bounded source data gate changed genuine DSP readiness");
}
}
int main(int argc,char**argv) {
 try {Run(argc,argv);std::cout<<"original static/MEM2 physical backing gate: "<<checks<<" checks; DSP engine/AX initialization remain HOLD\n";return 0;}
 catch(const std::exception&e){std::cerr<<"original backing gate: "<<e.what()<<'\n';return 1;}
}
