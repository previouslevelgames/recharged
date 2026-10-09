#include "platform/dsp_memory.h"
#include "platform/dsp_memory_abi.h"
#include "platform/dsp_mailbox.h"
#include "platform/interrupt_controller.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <SDL3/SDL_init.h>
namespace aurora {extern AuroraConfig g_config;}
void AuroraOSShutdown();
extern "C" {
#include <revolution/dsp.h>
extern u8 axDspSlave[];
extern u16 axDspSlaveLength;
}
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
std::atomic<unsigned> checks{};
void Check(bool result,const char* message) {++checks;if (!result) throw std::runtime_error(message);}
template<class F> void Throws(F&& function,const char* message) {
    bool rejected=false;try {function();}catch(const std::exception&) {rejected=true;}Check(rejected,message);
}
struct SDKLifetime {
    bool live{};
    bool memory_only{};
    void Close() {
        if (!live) return;
        if (memory_only) {AuroraOSShutdown();SDL_Quit();}
        else aurora_shutdown();
        live=false;
    }
    ~SDKLifetime() {Close();}
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
// Independently read the numeric native scalar, then select its big-endian
// byte. This deliberately does not call/copy the production XOR or segment
// conversion. Opaque PB tail bytes are byte data, not native integers.
unsigned char ScalarWireByte(NativeDSPMemoryEncoding encoding,const unsigned char* base,std::size_t wire) {
    std::size_t field=wire,width=1;
    switch (encoding) {
    case NativeDSPMemoryEncoding::RawBytes: break;
    case NativeDSPMemoryEncoding::NativeU16: width=2;field=(wire/2)*2;break;
    case NativeDSPMemoryEncoding::NativeU32: width=4;field=(wire/4)*4;break;
    case NativeDSPMemoryEncoding::AXParameterBlocks: {
        const auto record=(wire/320)*320,byte=wire-record;
        if (byte>=12 && byte<16) {width=4;field=record+12;}
        else if (byte<296) {width=2;field=record+(byte/2)*2;}
        break;
    }
    case NativeDSPMemoryEncoding::AXStudio: {
        const auto record=(wire/6)*6;
        if (wire-record<4) {width=4;field=record;}
        else {width=2;field=record+4;}
        break;
    }
    }
    if (width==1) return base[wire];
    if (width==2) {
        std::uint16_t value;std::memcpy(&value,base+field,2);
        return static_cast<unsigned char>(value>>(8*(1-(wire-field))));
    }
    std::uint32_t value;std::memcpy(&value,base+field,4);
    return static_cast<unsigned char>(value>>(8*(3-(wire-field))));
}
template<class Error,class F> void RejectRead(F&& function,const char* detail) {
    bool rejected=false;
    try {function();}
    catch(const Error& error) {
        rejected=true;Check(std::strstr(error.what(),detail)!=nullptr,"DSP read error precedence/detail changed");
    }
    Check(rejected,"DSP read error class changed or request unexpectedly succeeded");
}
void TypedReadOracle(NativeDSPMemoryEndpoint endpoint) {
    constexpr std::size_t guard=32,total=768;
    auto* storage=static_cast<unsigned char*>(OSAllocFromArenaLo(total,32));
    Check(storage!=nullptr,"actual SDK failed typed-read fixture allocation");
    auto* base=storage+guard;
    std::array<unsigned char,total> initial{};
    for (std::size_t i=0;i<total;++i) initial[i]=static_cast<unsigned char>(i*29+7);
    const NativeDSPMemoryEncoding encodings[]={NativeDSPMemoryEncoding::RawBytes,
        NativeDSPMemoryEncoding::NativeU16,NativeDSPMemoryEncoding::NativeU32,
        NativeDSPMemoryEncoding::AXParameterBlocks,NativeDSPMemoryEncoding::AXStudio};
    std::size_t disjoint_transfers=0,alias_transfers=0;
    for (const auto encoding:encodings) {
        const std::size_t pin_bytes=encoding==NativeDSPMemoryEncoding::AXParameterBlocks ? 640
                                  : encoding==NativeDSPMemoryEncoding::AXStudio ? 120 : 64;
        std::memcpy(storage,initial.data(),total);
        const auto pin=PinNativeDSPMemory(base,pin_bytes,false,encoding);
        const auto physical=OSCachedToPhysical(base);
        try {
            std::vector<std::size_t> offsets;
            if (encoding==NativeDSPMemoryEncoding::AXParameterBlocks) {
                offsets={0,1,2,3,7,11,12,13,14,15,16,17,293,294,295,296,297,318,319,
                         320,321,331,332,333,335,336,615,616,617,638,639};
            } else {
                for (std::size_t i=0;i<pin_bytes;++i) offsets.push_back(i);
            }
            for (const auto offset:offsets) {
                std::vector<std::size_t> lengths={0,1,2,3,4,5,7,8,15,16,31,32,319,320,321,pin_bytes-offset};
                std::sort(lengths.begin(),lengths.end());
                lengths.erase(std::unique(lengths.begin(),lengths.end()),lengths.end());
                for (const auto bytes:lengths) {
                    if (bytes>pin_bytes-offset) continue;
                    for (std::size_t alignment=0;alignment<4;++alignment) {
                        std::array<unsigned char,total> output,expected;
                        output.fill(0xe1);expected=output;
                        for (std::size_t i=0;i<bytes;++i)
                            expected[guard+alignment+i]=ScalarWireByte(encoding,initial.data()+guard,offset+i);
                        DSPBackendReadMemory(endpoint,physical+offset,output.data()+guard+alignment,bytes);
                        if (output!=expected)
                            std::cerr<<"DSP read encoding="<<static_cast<int>(encoding)<<" offset="<<offset
                                     <<" bytes="<<bytes<<" alignment="<<alignment<<'\n';
                        Check(output==expected,"DSP read bytes or output guards differ from independent scalar oracle");
                        Check(std::memcmp(storage,initial.data(),total)==0,"disjoint read mutated retained source/guards");
                        ++disjoint_transfers;
                    }
                }
            }
            DSPBackendReadMemory(endpoint,physical,nullptr,0);
            DSPBackendReadMemory(endpoint,physical+pin_bytes-1,nullptr,0);
            // Raw overlapping memcpy has no defined byte oracle. For typed
            // reads, simulate the exact historical ascending mutation order.
            if (encoding!=NativeDSPMemoryEncoding::RawBytes) {
                for (const auto offset:offsets) {
                    for (const std::size_t bytes:{1u,2u,3u,4u,7u,16u}) {
                        if (bytes>pin_bytes-offset) continue;
                        const std::ptrdiff_t deltas[]={-1,0,1,static_cast<std::ptrdiff_t>(bytes),-static_cast<std::ptrdiff_t>(bytes)};
                        for (const auto delta:deltas) {
                            const auto destination=static_cast<std::ptrdiff_t>(guard+offset)+delta;
                            Check(destination>=0 && static_cast<std::size_t>(destination)+bytes<=total,
                                  "alias oracle escaped its actual SDK allocation");
                            std::memcpy(storage,initial.data(),total);
                            auto expected=initial;
                            for (std::size_t i=0;i<bytes;++i)
                                expected[destination+i]=ScalarWireByte(encoding,expected.data()+guard,offset+i);
                            DSPBackendReadMemory(endpoint,physical+offset,storage+destination,bytes);
                            if (std::memcmp(storage,expected.data(),total)!=0)
                                std::cerr<<"DSP alias encoding="<<static_cast<int>(encoding)<<" offset="<<offset
                                         <<" bytes="<<bytes<<" delta="<<delta<<'\n';
                            Check(std::memcmp(storage,expected.data(),total)==0,
                                  "typed alias source/output/guards differ from original ascending scalar mutation");
                            ++alias_transfers;
                        }
                    }
                }
            }
            std::memcpy(storage,initial.data(),total);
            std::array<unsigned char,total> untouched;
            untouched.fill(0xd3);const auto expected=untouched;
            const NativeDSPMemoryEndpoint stale{endpoint.generation+1};
            RejectRead<std::logic_error>([&]{DSPBackendReadMemory(stale,physical,nullptr,1);},"endpoint is stale");
            RejectRead<std::invalid_argument>([&]{DSPBackendReadMemory(endpoint,0,nullptr,1);},"destination is null");
            RejectRead<std::invalid_argument>([&]{DSPBackendReadMemory(endpoint,physical,nullptr,1);},"destination is null");
            RejectRead<std::out_of_range>([&]{DSPBackendReadMemory(endpoint,0,untouched.data(),1);},"pinned");
            RejectRead<std::out_of_range>([&]{DSPBackendReadMemory(endpoint,physical-1,untouched.data(),1);},"pinned");
            RejectRead<std::out_of_range>([&]{DSPBackendReadMemory(endpoint,physical+pin_bytes,nullptr,0);},"pinned");
            RejectRead<std::out_of_range>([&]{DSPBackendReadMemory(endpoint,physical,untouched.data(),pin_bytes+1);},"pinned");
            RejectRead<std::out_of_range>([&]{DSPBackendReadMemory(endpoint,physical,untouched.data(),std::numeric_limits<std::size_t>::max());},"pinned");
            RejectRead<std::out_of_range>([&]{DSPBackendReadMemory(endpoint,0xfffffff0u,untouched.data(),32);},"pinned");
            RejectRead<std::invalid_argument>([&]{DSPBackendWriteMemory(endpoint,physical,untouched.data(),1);},"read-only pinned");
            Check(untouched==expected && std::memcmp(storage,initial.data(),total)==0,
                  "failed read/write touched output or source guards");
        } catch (...) {ReleaseNativeDSPMemory(pin);throw;}
        ReleaseNativeDSPMemory(pin);
        RejectRead<std::out_of_range>([&]{std::array<unsigned char,4> output{};
            DSPBackendReadMemory(endpoint,physical,output.data(),1);},"pinned");
    }
    std::cout<<"DSP independent byte oracle: "<<disjoint_transfers<<" disjoint reads, "
             <<alias_transfers<<" ascending alias reads through actual SDK pins\n";
}
// Native byte that receives device byte wire: the independent big-endian field
// position used by ScalarWireByte, without the production XOR mapping.
std::size_t ScalarNativeIndex(NativeDSPMemoryEncoding encoding,std::size_t wire) {
    std::size_t field=wire,width=1;
    switch (encoding) {
    case NativeDSPMemoryEncoding::RawBytes: break;
    case NativeDSPMemoryEncoding::NativeU16: width=2;field=(wire/2)*2;break;
    case NativeDSPMemoryEncoding::NativeU32: width=4;field=(wire/4)*4;break;
    case NativeDSPMemoryEncoding::AXParameterBlocks: {
        const auto record=(wire/320)*320,byte=wire-record;
        if (byte>=12 && byte<16) {width=4;field=record+12;}
        else if (byte<296) {width=2;field=record+(byte/2)*2;}
        break;
    }
    case NativeDSPMemoryEncoding::AXStudio: {
        const auto record=(wire/6)*6;
        if (wire-record<4) {width=4;field=record;}
        else {width=2;field=record+4;}
        break;
    }
    }
    // Big-endian byte k of a little-endian native field lives at width-1-k.
    return field+(width-1-(wire-field));
}
void TypedWriteOracle(NativeDSPMemoryEndpoint endpoint) {
    constexpr std::size_t guard=32,total=768;
    auto* storage=static_cast<unsigned char*>(OSAllocFromArenaLo(total,32));
    Check(storage!=nullptr,"actual SDK failed typed-write fixture allocation");
    auto* base=storage+guard;
    std::array<unsigned char,total> initial{};
    for (std::size_t i=0;i<total;++i) initial[i]=static_cast<unsigned char>(i*29+7);
    std::array<unsigned char,total> input{};
    for (std::size_t i=0;i<total;++i) input[i]=static_cast<unsigned char>(i*53+101);
    const NativeDSPMemoryEncoding encodings[]={NativeDSPMemoryEncoding::RawBytes,
        NativeDSPMemoryEncoding::NativeU16,NativeDSPMemoryEncoding::NativeU32,
        NativeDSPMemoryEncoding::AXParameterBlocks,NativeDSPMemoryEncoding::AXStudio};
    std::size_t disjoint_transfers=0,alias_transfers=0;
    for (const auto encoding:encodings) {
        const std::size_t pin_bytes=encoding==NativeDSPMemoryEncoding::AXParameterBlocks ? 640
                                  : encoding==NativeDSPMemoryEncoding::AXStudio ? 120 : 64;
        std::memcpy(storage,initial.data(),total);
        const auto pin=PinNativeDSPMemory(base,pin_bytes,true,encoding);
        const auto physical=OSCachedToPhysical(base);
        try {
            std::vector<std::size_t> offsets;
            if (encoding==NativeDSPMemoryEncoding::AXParameterBlocks) {
                offsets={0,1,2,3,7,11,12,13,14,15,16,17,293,294,295,296,297,318,319,
                         320,321,331,332,333,335,336,615,616,617,638,639};
            } else {
                for (std::size_t i=0;i<pin_bytes;++i) offsets.push_back(i);
            }
            for (const auto offset:offsets) {
                std::vector<std::size_t> lengths={0,1,2,3,4,5,7,8,15,16,31,32,319,320,321,640,pin_bytes-offset};
                std::sort(lengths.begin(),lengths.end());
                lengths.erase(std::unique(lengths.begin(),lengths.end()),lengths.end());
                for (const auto bytes:lengths) {
                    if (bytes>pin_bytes-offset) continue;
                    std::memcpy(storage,initial.data(),total);
                    auto expected=initial;
                    for (std::size_t i=0;i<bytes;++i)
                        expected[guard+ScalarNativeIndex(encoding,offset+i)]=input[i];
                    DSPBackendWriteMemory(endpoint,physical+offset,input.data(),bytes);
                    if (std::memcmp(storage,expected.data(),total)!=0)
                        std::cerr<<"DSP write encoding="<<static_cast<int>(encoding)<<" offset="<<offset
                                 <<" bytes="<<bytes<<'\n';
                    Check(std::memcmp(storage,expected.data(),total)==0,
                          "DSP write bytes or guards differ from independent scalar oracle");
                    ++disjoint_transfers;
                }
            }
            // Typed writes whose source aliases the pin keep the historical
            // ascending byte order: each byte is read after earlier writes.
            if (encoding!=NativeDSPMemoryEncoding::RawBytes) {
                for (const auto offset:offsets) {
                    for (const std::size_t bytes:{1u,2u,3u,4u,7u,16u}) {
                        if (bytes>pin_bytes-offset) continue;
                        const std::ptrdiff_t deltas[]={-1,0,1,static_cast<std::ptrdiff_t>(bytes),-static_cast<std::ptrdiff_t>(bytes)};
                        for (const auto delta:deltas) {
                            const auto from=static_cast<std::ptrdiff_t>(guard+offset)+delta;
                            Check(from>=0 && static_cast<std::size_t>(from)+bytes<=total,
                                  "alias write oracle escaped its actual SDK allocation");
                            std::memcpy(storage,initial.data(),total);
                            auto expected=initial;
                            for (std::size_t i=0;i<bytes;++i)
                                expected[guard+ScalarNativeIndex(encoding,offset+i)]=expected[from+i];
                            DSPBackendWriteMemory(endpoint,physical+offset,storage+from,bytes);
                            Check(std::memcmp(storage,expected.data(),total)==0,
                                  "typed alias write differs from original ascending scalar order");
                            ++alias_transfers;
                        }
                    }
                }
            }
        } catch (...) {ReleaseNativeDSPMemory(pin);throw;}
        ReleaseNativeDSPMemory(pin);
    }
    std::cout<<"DSP independent byte oracle: "<<disjoint_transfers<<" disjoint writes, "
             <<alias_transfers<<" ascending alias writes through actual SDK pins\n";
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
    const bool memory_only=argc>1 && std::strcmp(argv[1],"--memory-only")==0;
    if (memory_only) {
        // Same genuine OS-only setup used by the existing native clock/data
        // qualifiers: stage bank sizes, then OSInit allocates the real backing.
        Check(SDL_Init(0),"actual SDL initialization failed");
        aurora::g_config.mem1Size=config.mem1Size;aurora::g_config.mem2Size=config.mem2Size;
        sdk.live=true;sdk.memory_only=true;
    } else {
        const auto actual=aurora_initialize(argc,argv,&config);sdk.live=true;
        Check(actual.window!=nullptr,"actual SDK window unavailable");
    }
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
    TypedReadOracle(endpoint);
    TypedWriteOracle(endpoint);
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
    sdk.Close();
    Throws([&]{DSPBackendReadMemory(endpoint,0x4200,copy.data(),1);},"shutdown SDK backing still accessible");
    Check(OSGetArenaLo()==nullptr && OSGetMEM2ArenaLo()==nullptr,"actual SDK did not release its arenas");
    Check(DSPCheckInit()==FALSE,"memory transport changed original DSP initialization state");
}
}
int main(int argc,char** argv) {
    try {Run(argc,argv);std::cout<<"native DSP MEM1 source/wire fixture: "<<checks<<" checks; AX static/MEM2 remain explicit holds\n";return 0;}
    catch(const std::exception& error){std::cerr<<"DSP MEM1 gate: "<<error.what()<<'\n';return 1;}
}
