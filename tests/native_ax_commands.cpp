#include "platform/ax_command_service.h"
#include "platform/dsp_memory.h"
#include "platform/dsp_memory_abi.h"
#include "platform/ax_storage_abi.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
extern "C" {
#include <revolution/ax.h>
}
#include <array>
#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
unsigned checks{},completed{};
void Check(bool result,const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
template<class F> void Throws(F&& function,const char* message) {
    bool rejected=false;
    try { function(); } catch(const std::exception&) { rejected=true; }
    Check(rejected,message);
}
template<class F> F Load(SDL_SharedObject* image,const char* name) {
    auto* address=SDL_LoadFunction(image,name);
    if (!address) throw std::runtime_error(std::string("actual source symbol absent: ")+name);
    return reinterpret_cast<F>(address);
}
struct Source {
#define AX_FUNCTION(name) decltype(&::name) name;
    AX_FUNCTION(__AXAllocInit) AX_FUNCTION(__AXVPBInit) AX_FUNCTION(__AXAuxInit)
    AX_FUNCTION(__AXClInit) AX_FUNCTION(__AXSPBInit) AX_FUNCTION(__AXGetPBs)
    AX_FUNCTION(__AXNextFrame) AX_FUNCTION(__AXGetCommandListAddress)
    AX_FUNCTION(__AXGetCommandListCycles) AX_FUNCTION(AXSetMode)
    AX_FUNCTION(AXSetMasterVolume) AX_FUNCTION(AXSetAuxAReturnVolume)
    AX_FUNCTION(AXSetAuxBReturnVolume) AX_FUNCTION(AXSetAuxCReturnVolume)
    AX_FUNCTION(AXRegisterAuxACallback) AX_FUNCTION(AXRegisterAuxBCallback)
    AX_FUNCTION(AXRegisterAuxCCallback) AX_FUNCTION(__AXGetStudio)
    AX_FUNCTION(__AXGetStackHead) AX_FUNCTION(AXAcquireVoice) AX_FUNCTION(AXFreeVoice)
    AX_FUNCTION(AXSetVoiceMix) AX_FUNCTION(AXSetVoiceState) AX_FUNCTION(AXSetVoiceVe)
    AX_FUNCTION(__AXServiceVPB) AX_FUNCTION(__AXDepopVoice) AX_FUNCTION(__AXPrintStudio)
    AX_FUNCTION(ChargedAXGetCommandStorage) AX_FUNCTION(ChargedAXGetVoiceStorage)
    AX_FUNCTION(ChargedAXGetAuxStorage) AX_FUNCTION(ChargedAXGetCompressorStorage)
#undef AX_FUNCTION
    explicit Source(SDL_SharedObject* image) {
#define AX_FUNCTION(name) name=Load<decltype(name)>(image,#name);
        AX_FUNCTION(__AXAllocInit) AX_FUNCTION(__AXVPBInit) AX_FUNCTION(__AXAuxInit)
        AX_FUNCTION(__AXClInit) AX_FUNCTION(__AXSPBInit) AX_FUNCTION(__AXGetPBs)
        AX_FUNCTION(__AXNextFrame) AX_FUNCTION(__AXGetCommandListAddress)
        AX_FUNCTION(__AXGetCommandListCycles) AX_FUNCTION(AXSetMode)
        AX_FUNCTION(AXSetMasterVolume) AX_FUNCTION(AXSetAuxAReturnVolume)
        AX_FUNCTION(AXSetAuxBReturnVolume) AX_FUNCTION(AXSetAuxCReturnVolume)
        AX_FUNCTION(AXRegisterAuxACallback) AX_FUNCTION(AXRegisterAuxBCallback)
        AX_FUNCTION(AXRegisterAuxCCallback) AX_FUNCTION(__AXGetStudio)
        AX_FUNCTION(__AXGetStackHead) AX_FUNCTION(AXAcquireVoice) AX_FUNCTION(AXFreeVoice)
        AX_FUNCTION(AXSetVoiceMix) AX_FUNCTION(AXSetVoiceState) AX_FUNCTION(AXSetVoiceVe)
        AX_FUNCTION(__AXServiceVPB) AX_FUNCTION(__AXDepopVoice) AX_FUNCTION(__AXPrintStudio)
        AX_FUNCTION(ChargedAXGetCommandStorage) AX_FUNCTION(ChargedAXGetVoiceStorage)
        AX_FUNCTION(ChargedAXGetAuxStorage) AX_FUNCTION(ChargedAXGetCompressorStorage)
#undef AX_FUNCTION
    }
};
struct SDK {
    bool live{};
    ~SDK() { if (live) aurora_shutdown(); }
};
struct Image {
    std::string path;
    SDL_SharedObject* initial{};
    std::vector<SDL_SharedObject*> leases;
    explicit Image(const char* p):path(std::filesystem::absolute(p).string()) {
        initial=SDL_LoadObject(path.c_str());
        if (!initial) throw std::runtime_error(std::string("actual six-TU source image could not load: ")+SDL_GetError());
        ++checks;
    }
    ~Image() { if (initial) SDL_UnloadObject(initial); }
    static BOOL Retain(void* context) noexcept {
        auto& self=*static_cast<Image*>(context);
        auto* image=SDL_LoadObject(self.path.c_str());
        if (!image) return FALSE;
        try { self.leases.push_back(image); }
        catch (...) { SDL_UnloadObject(image); return FALSE; }
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& self=*static_cast<Image*>(context);
        auto* image=self.leases.back(); self.leases.pop_back(); SDL_UnloadObject(image);
    }
};
struct Backings {
    std::vector<OSNativeStaticMemory> statics;
    std::vector<NativeDSPMemoryPin> pins;
    bool attached{};
    ~Backings() {
        for(auto it=pins.rbegin();it!=pins.rend();++it) ReleaseNativeDSPMemory(*it);
        if (attached) DetachNativeDSPMEM1();
        for(auto it=statics.rbegin();it!=statics.rend();++it) OSNativeReleaseStaticMemory(*it);
    }
    u32 Map(Image& image,const char* identity,const char* resource,ChargedAXStorage storage,bool writable) {
        Check(storage.address && storage.bytes,"original storage has no actual extent");
        Check(reinterpret_cast<uintptr_t>(storage.address)>0xffffffffULL,"source statics must exercise LP64 pointer width");
        OSNativeStaticMemoryOwner owner{identity,resource,storage.address,storage.bytes,writable?TRUE:FALSE,
                                       &image,Image::Retain,Image::Release};
        const auto before=reinterpret_cast<uintptr_t>(OSGetArenaHi());
        const auto memory=OSNativeRegisterStaticMemory(&owner);
        statics.push_back(memory);
        Check(memory.physical_address==before-reinterpret_cast<uintptr_t>(OSPhysicalToCached(0))-
              ((storage.bytes+31)&~u32(31)),"physical reservation differs from independent aligned arena-high oracle");
        Check(OSPhysicalToCached(memory.physical_address)==storage.address,"source physical inverse lost its retained backing");
        Check(OSCachedToPhysical(storage.address)==memory.physical_address,"source pointer was replaced by a transient/truncated token");
        return memory.physical_address;
    }
    void Pin(ChargedAXStorage storage,bool writable,NativeDSPMemoryEncoding encoding) {
        pins.push_back(PinNativeDSPMemory(storage.address,storage.bytes,writable,encoding));
    }
};
u16 BE16(const unsigned char* p) { return (u16(p[0])<<8)|p[1]; }
u32 BE32(const unsigned char* p) { return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3]; }
void Address(std::vector<u16>& words,u32 value) {words.push_back(value>>16);words.push_back(value);}
std::vector<unsigned char> Wire(const std::vector<u16>& words) {
    std::vector<unsigned char> result;
    for(auto word:words) {result.push_back(word>>8);result.push_back(word);}
    return result;
}
unsigned aux_calls{};
void AuxRecorder(void*,void*) {++aux_calls;}

template<class F> void AXReject(F&& function,NativeAXFailure failure,const char* message) {
    bool rejected=false;
    try {function();} catch(const NativeAXCommandError& error){rejected=error.failure()==failure;}
    Check(rejected,message);
}
void Run(int argc,char** argv) {
    Check(argc==3,"requires actual unchanged six-whole-TU AX image and SHA256 identity");
    SDK sdk;
    const auto directory=std::filesystem::absolute("native-ax-command-data").string();
    std::filesystem::create_directories(directory);
    AuroraConfig config{};config.appName="Original AX bounded commands";
    config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
    config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;
    config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual CPU SDK initialization failed");
    sdk.live=true;OSInit();
    Image image(argv[1]);Source source(image.initial);Backings backing;
    const auto commands=source.ChargedAXGetCommandStorage();ChargedAXStorage voice[2],aux[3];
    source.ChargedAXGetVoiceStorage(voice);source.ChargedAXGetAuxStorage(aux);
    const auto compressor=source.ChargedAXGetCompressorStorage();
    ChargedAXStorage studio{source.__AXGetStudio(),sizeof(AXSTUDIO)};
    const auto cl=backing.Map(image,argv[2],"Commands",commands,true);
    const auto pb=backing.Map(image,argv[2],"PBs",voice[0],true);
    backing.Map(image,argv[2],"ITD",voice[1],true);
    for(unsigned i=0;i<3;++i)backing.Map(image,argv[2],i==0?"AuxA":i==1?"AuxB":"AuxC",aux[i],true);
    backing.Map(image,argv[2],"Compressor",compressor,false);
    backing.Map(image,argv[2],"Studio",studio,false);
    const auto endpoint=AttachNativeDSPMEM1();backing.attached=true;
    backing.Pin(commands,true,NativeDSPMemoryEncoding::NativeU16);
    backing.Pin(voice[0],true,NativeDSPMemoryEncoding::AXParameterBlocks);
    backing.Pin(voice[1],true,NativeDSPMemoryEncoding::RawBytes);
    for(auto owner:aux)backing.Pin(owner,true,NativeDSPMemoryEncoding::NativeU32);
    backing.Pin(compressor,false,NativeDSPMemoryEncoding::NativeU16);
    backing.Pin(studio,false,NativeDSPMemoryEncoding::AXStudio);
    SDL_UnloadObject(image.initial);image.initial=nullptr;
    const auto physical=OSGetArenaHi();OSAllocFromArenaLo(32,32);
    Check(OSGetArenaHi()==physical,"actual static owners changed after source arena capture");
    auto* surround=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(800,32));
    auto* lr=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(416,32));
    auto* remote=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(256,32));
    backing.Pin({surround,800},true,NativeDSPMemoryEncoding::NativeU32);
    backing.Pin({lr,416},true,NativeDSPMemoryEncoding::NativeU16);
    backing.Pin({remote,256},true,NativeDSPMemoryEncoding::NativeU16);
    void* remotes[4]{remote,remote+64,remote+128,remote+192};
    const auto sb=OSCachedToPhysical(surround),lb=OSCachedToPhysical(lr);
    source.__AXAllocInit();source.__AXVPBInit();source.__AXSPBInit();source.__AXPrintStudio();
    Check(source.__AXGetPBs()==voice[0].address,"real source PB owner changed");
    auto Prepare=[&](unsigned mode) {
        source.__AXClInit();source.__AXAuxInit();source.AXSetMode(mode);
        std::memset(surround,0,mode==AX_OUTPUT_DPL2?768:384);
        std::memset(surround+(mode==AX_OUTPUT_DPL2?768:384),0xb5,800-(mode==AX_OUTPUT_DPL2?768:384));
        std::memset(lr,0x9a,416);std::memset(remote,0x6c,256);
        source.__AXNextFrame(surround,lr,remotes);
        return OSCachedToPhysical(source.__AXGetCommandListAddress());
    };
    std::array<bool,15> covered{};
    auto Observe=[&](const NativeAXCommandList& list){for(unsigned i=0;i<list.command_count;++i)covered[static_cast<unsigned>(list.commands[i].opcode)]=true;};
    // Original list producer chooses each native buffer/volume/order/cycle count.
    // A separate zero-vector oracle is exact integer zero under every signed
    // route, saturator, master gain and fresh compressor multiply; no other
    // sample contribution is accepted by the device slice.
    for(unsigned mode=0;mode<3;++mode)for(unsigned frame=0;frame<2;++frame) {
        const auto list=Prepare(mode);
        const auto parsed=ReadNativeAXCommandList(endpoint,list,128);Observe(parsed);
        Check(parsed.command_count==7 && parsed.consumed_words==30,"source fresh zero list shape changed");
        Check(parsed.commands[0].opcode==NativeAXOpcode::Setup &&
              parsed.commands[1].opcode==static_cast<NativeAXOpcode>(mode+1) &&
              parsed.commands[2].opcode==NativeAXOpcode::Process &&
              parsed.commands[3].opcode==NativeAXOpcode::Compressor &&
              parsed.commands[4].opcode==NativeAXOpcode::RemoteOutput &&
              parsed.commands[5].opcode==(mode==2?NativeAXOpcode::OutputDPL2:NativeAXOpcode::Output) &&
              parsed.commands[6].opcode==NativeAXOpcode::End,"actual source opcodes/order differ from protocol");
        Check(parsed.commands[3].arguments[0]==32768 && parsed.commands[3].arguments[1]==10,
              "actual startup compressor request was omitted or changed");
        const auto result=ExecuteNativeAXZeroInputFrame(endpoint,list,128);++completed;
        Check(result.stopped_voices==96 && result.stereo_frames==96 &&
              result.remote_samples_per_channel==18 && result.compressor_present && result.dpl2==(mode==2),
              "real zero-block work/voice metadata differs from source contract");
        Check(result.written_bytes==(mode==2?1296u:912u),"actual output extent differs from96 stereo/18 remote sample contract");
        Check(std::all_of(lr,lr+384,[](unsigned char x){return x==0;}) &&
              std::all_of(lr+384,lr+416,[](unsigned char x){return x==0x9a;}),
              "device stereo output is not exact zero or wrote its guard");
        Check(std::all_of(surround,surround+(mode==2?768:384),[](unsigned char x){return x==0;}) &&
              std::all_of(surround+(mode==2?768:384),surround+800,[](unsigned char x){return x==0xb5;}),
              "device surround writes changed untouched owner suffix");
        for(unsigned i=0;i<4;++i)Check(std::all_of(remote+i*64,remote+i*64+36,[](unsigned char x){return x==0;}) &&
              std::all_of(remote+i*64+36,remote+(i+1)*64,[](unsigned char x){return x==0x6c;}),
              "actual remote channel36-byte output/guard changed");
    }
    const auto list=Prepare(0);
    std::array<unsigned char,128> original{};
    DSPBackendReadMemory(endpoint,list,original.data(),original.size());
    auto WireCommand=[&](unsigned word,std::initializer_list<unsigned char> value) {
        DSPBackendWriteMemory(endpoint,list+word*2,value.begin(),value.size());
    };
    auto Reset=[&]{DSPBackendWriteMemory(endpoint,list,original.data(),original.size());};
    auto Unwritten=[&]{Check(std::all_of(lr,lr+416,[](unsigned char x){return x==0x9a;}) &&
        std::all_of(remote,remote+256,[](unsigned char x){return x==0x6c;}),"rejected device job partially published output");};
    WireCommand(29,{0x7f,0xff});
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,list,128);},NativeAXFailure::UnknownCommand,"unknown END-tail command was silently accepted");Unwritten();Reset();
    AXReject([&]{ReadNativeAXCommandList(endpoint,list,58);},NativeAXFailure::MissingEnd,"missing source END was accepted");
    for(const auto& cmd:ReadNativeAXCommandList(endpoint,list,128).commands) {
        if(cmd.opcode==NativeAXOpcode::End)break;
        AXReject([&]{ReadNativeAXCommandList(endpoint,list,(cmd.word_offset+1)*2);},
                 NativeAXFailure::TruncatedCommand,"published cut inside actual source command did not reject");
    }
    AXReject([&]{ReadNativeAXCommandList(endpoint,list,129);},NativeAXFailure::InvalidListExtent,"list crossed original128-byte slot");
    AXReject([&]{ReadNativeAXCommandList(endpoint,list+1,128);},NativeAXFailure::InvalidListExtent,"unaligned list gained a guessed format");
    WireCommand(27,{0,0,0,1});
    Throws([&]{ExecuteNativeAXZeroInputFrame(endpoint,list,128);},"unrepresented output address was accepted");Unwritten();Reset();
    // The real source creates a running voice; the device must fail rather than
    // replacing that voice with silence or advancing sample/envelope state.
    auto* active=source.AXAcquireVoice(7,nullptr,0);
    source.AXSetVoiceState(active,AX_VOICE_RUN);active->sync=AX_PBSYNC_ALL;
    source.__AXServiceVPB(active);
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,list,128);},NativeAXFailure::ActiveVoice,"active source voice was rendered as silence");Unwritten();
    Check(source.__AXGetPBs()[active->index].state==AX_VOICE_RUN,"device rejection changed source voice state");
    source.AXSetVoiceState(active,AX_VOICE_STOP);active->sync=AX_PBSYNC_ALL;
    source.__AXServiceVPB(active);source.AXFreeVoice(active);
    AXPB depop{};depop.dpop.aL=1920;source.__AXDepopVoice(&depop);source.__AXPrintStudio();
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,list,128);},NativeAXFailure::NonzeroStudio,"real depop contribution was silently discarded");Unwritten();
    source.__AXSPBInit();source.__AXPrintStudio();
    const std::array<unsigned char,4> nonzero{0,0,0,1};
    DSPBackendWriteMemory(endpoint,sb,nonzero.data(),4);
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,list,128);},NativeAXFailure::NonzeroSurround,"surround contributor was silently discarded");Unwritten();
    const std::array<unsigned char,4> zero{};DSPBackendWriteMemory(endpoint,sb,zero.data(),4);
    const std::array<unsigned char,4> cycle{static_cast<unsigned char>(pb>>24),static_cast<unsigned char>(pb>>16),
                                            static_cast<unsigned char>(pb>>8),static_cast<unsigned char>(pb)};
    std::array<unsigned char,4> old_next{};DSPBackendReadMemory(endpoint,pb,old_next.data(),4);
    DSPBackendWriteMemory(endpoint,pb,cycle.data(),4);
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,list,128);},NativeAXFailure::InvalidVoiceChain,"cyclic original PB data was accepted");Unwritten();
    DSPBackendWriteMemory(endpoint,pb,old_next.data(),4);
    // Actual source AUX registration produces the real native command branches.
    source.AXRegisterAuxACallback(AuxRecorder,nullptr);
    source.__AXNextFrame(surround,lr,remotes);
    const auto aux_list=OSCachedToPhysical(source.__AXGetCommandListAddress());
    const auto parsed_aux=ReadNativeAXCommandList(endpoint,aux_list,128);Observe(parsed_aux);
    Check(parsed_aux.commands[3].opcode==NativeAXOpcode::MixAuxA,"actual source AUX request disappeared");
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,aux_list,128);},NativeAXFailure::AuxiliaryProcessing,"actual source AUX effect gained a fake silent success");Unwritten();
    source.AXRegisterAuxBCallback(AuxRecorder,nullptr);source.AXRegisterAuxCCallback(AuxRecorder,nullptr);
    for(unsigned mode=0;mode<3;++mode) {
        std::memset(surround,0,768);source.AXSetMode(mode);source.__AXNextFrame(surround,lr,remotes);
        const auto full_aux=OSCachedToPhysical(source.__AXGetCommandListAddress());
        const auto parsed=ReadNativeAXCommandList(endpoint,full_aux,128);Observe(parsed);
        Check(parsed.command_count==(mode==2?9:10) && parsed.consumed_words==(mode==2?58:48),
              "full actual AUX command list consumed incorrect native protocol lengths");
        if(mode==2) {
            Check(parsed.commands[3].opcode==NativeAXOpcode::UploadAuxAMixLRSC && parsed.commands[3].argument_count==13 &&
                  parsed.commands[4].opcode==NativeAXOpcode::UploadAuxBMixLRSC && parsed.commands[4].argument_count==13,
                  "original DPL2 full address upload command was guessed as older firmware");
        } else {
            Check(parsed.commands[3].opcode==NativeAXOpcode::MixAuxA && parsed.commands[3].argument_count==5 &&
                  parsed.commands[4].opcode==NativeAXOpcode::MixAuxB && parsed.commands[5].opcode==NativeAXOpcode::MixAuxC,
                  "original three effect-bus commands were not fully parsed");
        }
        AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,full_aux,128);},NativeAXFailure::AuxiliaryProcessing,
                 "source AUX/DPL2 contributor gained an unimplemented successful frame");Unwritten();
    }
    Check(std::all_of(covered.begin(),covered.end(),[](bool x){return x;}),"not all15 actual source command shapes were exercised");
    Check(aux_calls==0,"device slice directly invoked original AUX callback");
    // One actual device thread uses the same owner-retained bus; no raw pointer
    // or source callback crosses the operation. Join precedes lifetime retirement.
    const auto worker_list=Prepare(0);
    DSPBackendReadMemory(endpoint,worker_list,original.data(),original.size());
    const auto compressor_bus=OSCachedToPhysical(compressor.address);
    const std::array<unsigned char,4> readonly_address{static_cast<unsigned char>(compressor_bus>>24),
        static_cast<unsigned char>(compressor_bus>>16),static_cast<unsigned char>(compressor_bus>>8),static_cast<unsigned char>(compressor_bus)};
    DSPBackendWriteMemory(endpoint,worker_list+27*2,readonly_address.data(),4);
    Throws([&]{ExecuteNativeAXZeroInputFrame(endpoint,worker_list,128);},"read-only final output allowed earlier remote output publication");Unwritten();
    DSPBackendWriteMemory(endpoint,worker_list,original.data(),original.size());
    const std::array<unsigned char,4> null_next{};
    DSPBackendReadMemory(endpoint,pb,old_next.data(),4);DSPBackendWriteMemory(endpoint,pb,null_next.data(),4);
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,worker_list,128);},NativeAXFailure::InvalidVoiceChain,
             "uninitialized/missing voice chain was treated as a silent successful frame");Unwritten();
    DSPBackendWriteMemory(endpoint,pb,old_next.data(),4);
    const std::array<unsigned char,2> changed_threshold{0,0};DSPBackendWriteMemory(endpoint,worker_list+10*2,changed_threshold.data(),2);
    AXReject([&]{ExecuteNativeAXZeroInputFrame(endpoint,worker_list,128);},NativeAXFailure::UnsupportedCompressor,
             "unknown compressor policy gained a no-op success");Unwritten();
    DSPBackendWriteMemory(endpoint,worker_list,original.data(),original.size());
    std::exception_ptr worker_error;
    std::thread worker([&]{try{ExecuteNativeAXZeroInputFrame(endpoint,worker_list,128);}catch(...){worker_error=std::current_exception();}});
    worker.join();if(worker_error)std::rethrow_exception(worker_error);++completed;++checks;
    Check(std::all_of(lr,lr+384,[](unsigned char x){return x==0;}),"real device-thread output was not visible after completion");
    Throws([&]{ReadNativeAXCommandList({endpoint.generation+1},cl,128);},"stale bus generation was accepted");
    std::cout<<"Original AX bounded command service: "<<checks<<" checks, "<<completed
             <<" zero-input frames; active/AUX/depop/bootstrap readiness remains unavailable\n";
}
}
int main(int argc,char** argv) {
    try{Run(argc,argv);return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
