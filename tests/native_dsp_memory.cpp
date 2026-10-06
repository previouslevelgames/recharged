#include "platform/dsp_memory.h"
#include "platform/dsp_memory_abi.h"
#include "platform/dsp_mailbox.h"
#include "platform/interrupt_controller.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
extern "C" {
#include <revolution/dsp.h>
extern u8 axDspSlave[];
extern u16 axDspSlaveLength;
}
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

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
void Run(int argc,char** argv) {
    Check(DSPCheckInit()==FALSE,"original DSP already initialized");
    Throws([]{AttachNativeDSPMEM1();},"uninitialized endpoint invented real MEM1 backing");
    SDKLifetime sdk;
    const auto data=std::filesystem::absolute("dsp-memory-sdk-data").string();
    std::filesystem::create_directories(data);
    AuroraConfig config{};
    config.appName="Native DSP memory hardware qualifier";
    config.userPath=config.cachePath=data.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
    config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;
    config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    const auto actual=aurora_initialize(argc,argv,&config);sdk.live=true;
    Check(actual.window!=nullptr,"actual SDK window unavailable");
    OSInit();
    Check(OSGetPhysicalMemSize()==24u*1024u*1024u,"actual configured MEM1 size changed");
    auto endpoint=AttachNativeDSPMEM1();
    Check(AttachNativeDSPMEM1().generation==endpoint.generation,"idempotent address attachment changed owner");
    auto* memory=static_cast<u8*>(OSPhysicalToCached(0));
    auto* iram=static_cast<u8*>(OSAllocFromArenaLo(512,32));
    auto* dram=static_cast<u8*>(OSAllocFromArenaLo(128,32));
    Check(reinterpret_cast<std::uintptr_t>(iram)>0xffffffffULL,"fixture did not exercise a real high native pointer");
    std::cout<<"actual SDK MEM1 base alignment="<<(reinterpret_cast<std::uintptr_t>(memory)&31)<<" IRAM physical offset="<<OSCachedToPhysical(iram)<<" DRAM physical offset="<<OSCachedToPhysical(dram)<<"\n";
    Check(iram==memory+0x4000 && dram==memory+0x4200,"actual canonical arena/word oracle changed");
    std::array<u8,512> expected{};
    std::array<u8,128> zero_context{};
    Check(std::memcmp(iram,expected.data(),expected.size())==0 && std::memcmp(dram,zero_context.data(),zero_context.size())==0,
          "actual aligned SDK MEM1 allocation lost original zero initialization");
    for(unsigned i=0;i<expected.size();++i)expected[i]=static_cast<u8>((i*37+19)&255);
    std::memcpy(iram,expected.data(),expected.size());std::memset(dram,0xa5,128);
    Throws([&]{ChargedDSPTaskMemoryWord(iram,512,0);},"unregistered source pointer produced a successful DSP word");
    auto source=PinNativeDSPMEM1(iram,512,false);auto context=PinNativeDSPMEM1(dram,128,true);
    Check(ChargedDSPTaskMemoryWord(iram,512,0)==0x4000,"source high address not converted through canonical MEM1");
    Check(ChargedDSPTaskMemoryWord(dram,128,1)==0x4200,"writable context physical word differs");
    Check(ChargedDSPTaskMemoryWord(nullptr,0,0)==0,"original empty task field changed");
    Check(ChargedDSPTaskMemoryWord(iram+511,1,0)==0x41ff,"last real pinned byte rejected");
    Throws([&]{ChargedDSPTaskMemoryWord(iram,513,0);},"source length crossed pinned object");
    Check(ChargedDSPTaskMemoryWord(iram+512,1,0)==0x4200,"adjacent actual context pin was confused with unowned memory");
    Throws([&]{ChargedDSPTaskMemoryWord(dram+128,1,0);},"one-past final pinned object accepted");
    Throws([&]{ChargedDSPTaskMemoryWord(iram,1,1);},"source output was allowed into readonly input");
    Throws([&]{ChargedDSPTaskMemoryWord(iram,1,2);},"unknown direction succeeded");
    Throws([&]{PinNativeDSPMEM1(iram+16,8,false);},"overlapping source object pin silently accepted");
    Throws([&]{PinNativeDSPMEM1(memory+24u*1024u*1024u-1,2,true);},"pin crossed actual MEM1 allocation");
    Throws([&]{PinNativeDSPMEM1(nullptr,1,false);},"null source object got a physical carrier");
    Throws([&]{PinNativeDSPMEM1(iram,0,false);},"empty fake owner pin accepted");

    std::array<u8,512> copy{};
    DSPBackendReadMemory(endpoint,0x4000,copy.data(),copy.size());
    Check(copy==expected,"actual device read changed raw byte order/content");
    std::array<u8,128> output{};
    for(unsigned i=0;i<output.size();++i)output[i]=static_cast<u8>((i*11+7)&255);
    DSPBackendWriteMemory(endpoint,0x4200,output.data(),output.size());
    Check(std::memcmp(dram,output.data(),output.size())==0,"actual device write did not reach genuine source backing");
    Throws([&]{DSPBackendWriteMemory(endpoint,0x4000,output.data(),1);},"device wrote readonly IRAM source");
    Throws([&]{DSPBackendReadMemory(endpoint,0x41ff,copy.data(),2);},"device read crossed pinned owner extent");
    Throws([&]{DSPBackendReadMemory(endpoint,0xfffffff0,copy.data(),32);},"wrapping physical address was accepted");
    Throws([&]{DSPBackendReadMemory(endpoint,0x80004000,copy.data(),1);},"mailbox status bit guessed into a memory address");
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            std::array<u8,512> local{};DSPBackendReadMemory(endpoint,0x4000,local.data(),local.size());
            Check(local==expected,"device worker failed real pinned transfer");
            Throws([&]{PinNativeDSPMEM1(iram,1,false);},"device worker registered source ownership");
            Throws([&]{ChargedDSPTaskMemoryWord(iram,1,0);},"device worker executed source address conversion");
        }catch(...){failure=std::current_exception();}
    });worker.join();if(failure)std::rethrow_exception(failure);

    InitializeNativeInterruptController();auto device=AttachNativeDSPMailboxes();
    DSPTask previous{},next{};
    previous.dramMmemAddr=dram;previous.dramMmemLen=128;previous.dramDspAddr=0x0cd2;
    next.iramMmemAddr=iram;next.iramMmemLen=512;next.iramDspAddr=0;
    next.startVector=0x10;next.resumeVector=0x30;next.state=DSP_TASK_STATE_0;
    next.dramMmemAddr=dram;next.dramMmemLen=128;next.dramDspAddr=0x0cd2;
    constexpr std::array<u32,10> initial{0x80004200,0x80000080,0x80000cd2,
        0x80004000,0x80000200,0x80000000,0x80000010,0x80000000,0x80000000,0x80000000};
    SourceWireGate(device,&previous,&next,initial);
    next.state=DSP_TASK_STATE_2;
    constexpr std::array<u32,10> resume{0x80000000,0x80000000,0x80000000,
        0x80004000,0x80000200,0x80000000,0x80000030,0x80004200,0x80000080,0x80000cd2};
    SourceWireGate(device,nullptr,&next,resume);
    Check(DSPCheckInit()==FALSE,"actual task word transfer fabricated DSP initialization");

    // Actual AX data is a native module static. Fail honestly: it is not moved
    // into a guessed physical offset or accepted as a transient address token.
    Check(axDspSlaveLength>0,"actual original DSPCode source did not supply its authored extent");
    Check(reinterpret_cast<std::uintptr_t>(axDspSlave)>0xffffffffULL,"actual static fixture did not exercise full pointer width");
    Throws([&]{PinNativeDSPMEM1(axDspSlave,axDspSlaveLength,false);},"static DSP code was mapped into guessed MEM1 ownership");
    Throws([&]{ChargedDSPTaskMemoryWord(axDspSlave,axDspSlaveLength,0);},"static AX input got an invented address carrier");
    auto* mem2=OSGetMEM2ArenaLo();
    Throws([&]{PinNativeDSPMEM1(mem2,32,true);},"MEM2 was confused with canonical MEM1");
    ReleaseNativeDSPMemory(source);
    Throws([&]{DSPBackendReadMemory(endpoint,0x4000,copy.data(),1);},"released source pin retained device access");
    Throws([&]{ChargedDSPTaskMemoryWord(iram,1,0);},"released source object retained address conversion");
    Throws([&]{ReleaseNativeDSPMemory(source);},"double release accepted an unknown owner");
    const auto replacement=PinNativeDSPMEM1(iram,512,false);
    Check(replacement.identity!=source.identity,"repin reused stale ownership handle");
    Throws([&]{ReleaseNativeDSPMemory(source);},"stale released handle removed newly pinned owner");
    ReleaseNativeDSPMemory(replacement);ReleaseNativeDSPMemory(context);
    const auto old_endpoint=endpoint;DetachNativeDSPMEM1();
    Throws([&]{DSPBackendReadMemory(old_endpoint,0x4200,copy.data(),1);},"detached endpoint retained native backing");
    endpoint=AttachNativeDSPMEM1();
    Check(endpoint.generation!=old_endpoint.generation,"reattach reused device generation");
    Throws([&]{DSPBackendReadMemory(old_endpoint,0x4200,copy.data(),1);},"old device endpoint accessed reattached space");
    Throws([&]{ReleaseNativeDSPMemory(context);},"old pin survived source address-space replacement");
    DetachNativeDSPMEM1();DetachNativeDSPMailboxes();ShutdownNativeInterruptController();
    aurora_shutdown();sdk.live=false;
    Throws([&]{DSPBackendReadMemory(endpoint,0x4200,copy.data(),1);},"shutdown SDK backing still accessible");
    Check(OSGetArenaLo()==nullptr && OSGetMEM2ArenaLo()==nullptr,"actual SDK did not release its arenas");
    Check(DSPCheckInit()==FALSE,"memory transport changed original DSP initialization state");
}
}
int main(int argc,char** argv) {
    try {Run(argc,argv);std::cout<<"native DSP MEM1 source/wire fixture: "<<checks<<" checks; AX static/MEM2 remain explicit holds\n";return 0;}
    catch(const std::exception& error){std::cerr<<"DSP MEM1 gate: "<<error.what()<<'\n';return 1;}
}
