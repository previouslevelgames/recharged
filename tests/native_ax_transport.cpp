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
unsigned checks{};
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

void CheckCommands(Source& source,NativeDSPMemoryEndpoint endpoint,u32 command_base,u32 studio,u32 pb,
                   const std::array<u32,3>& aux,u32 compressor,void* surround,void* lr,void* remotes[4]) {
    const auto surround_bus=OSCachedToPhysical(surround),lr_bus=OSCachedToPhysical(lr);
    std::array<u32,4> remote_bus{};
    for(unsigned i=0;i<4;++i) remote_bus[i]=OSCachedToPhysical(remotes[i]);
    // Authored protocol variants: exact numeric opcodes and field order. Source
    // state/list selection is independently checked; no command parser/mixer.
    for(unsigned variant=0;variant<4;++variant) {
        source.__AXClInit();source.__AXAuxInit();
        const bool effects=variant!=0;
        const bool dpl2=variant==3;
        source.AXSetMode(variant==2?AX_OUTPUT_SURROUND:dpl2?AX_OUTPUT_DPL2:AX_OUTPUT_STEREO);
        source.AXSetMasterVolume(0xffff); // Source clamps only this volume.
        source.AXSetAuxAReturnVolume(0x1234);source.AXSetAuxBReturnVolume(0x4321);
        source.AXSetAuxCReturnVolume(0xfedc);
        if(effects) {
            source.AXRegisterAuxACallback(AuxRecorder,nullptr);
            source.AXRegisterAuxBCallback(AuxRecorder,nullptr);
            source.AXRegisterAuxCCallback(AuxRecorder,nullptr);
        }
        std::vector<u16> expected{0};Address(expected,studio);
        expected.push_back(variant==2?2:dpl2?3:1);Address(expected,surround_bus);
        expected.push_back(4);Address(expected,pb);
        if(variant==1 || variant==2) {
            for(unsigned i=0;i<3;++i) {
                expected.push_back(5+i);expected.push_back(i==0?0x1234:i==1?0x4321:0xfedc);
                Address(expected,aux[i]);Address(expected,aux[i]+(i==2?1152:1536));
            }
        } else if(dpl2) {
            for(unsigned i=0;i<2;++i) {
                expected.push_back(8+i);expected.push_back(i==0?0x1234:0x4321);
                for(auto offset:{0u,1152u,1536u,1920u,2304u,2688u})Address(expected,aux[i]+offset);
            }
        }
        expected.insert(expected.end(),{10,32768,10});Address(expected,compressor);
        expected.push_back(13);for(auto word:remote_bus)Address(expected,word);
        expected.push_back(dpl2?12:11);expected.push_back(32768);
        Address(expected,surround_bus);Address(expected,lr_bus);expected.push_back(14);
        const unsigned expected_cycles=7811+4126+(variant==2?829:dpl2?925:733)+
            (variant==1||variant==2?3*2235:dpl2?2*3036:0)+1850+409+(dpl2?1195:1172)+30;
        const auto bytes=Wire(expected);
        Check(bytes.size()<=128,"independent command oracle exceeds original slot");
        for(unsigned slot=0;slot<2;++slot) {
            source.__AXNextFrame(surround,lr,remotes);
            auto* address=source.__AXGetCommandListAddress();
            Check(OSCachedToPhysical(address)==command_base+slot*128,"original double-buffer selection changed");
            std::array<unsigned char,136> actual;actual.fill(0xcd);
            DSPBackendReadMemory(endpoint,command_base+slot*128,actual.data()+4,128);
            Check(std::equal(bytes.begin(),bytes.end(),actual.begin()+4),"actual original command bytes/physical fields differ from protocol oracle");
            Check(actual[0]==0xcd && actual[3]==0xcd && actual[132]==0xcd && actual[135]==0xcd,"command DMA copy crossed its actual extent");
            Check(source.__AXGetCommandListCycles()==expected_cycles,"original AX command cycle accounting changed");
        }
    }
    Check(aux_calls==0,"command storage gate unexpectedly executed an effect callback");
}

void CheckPB(Source& source,NativeDSPMemoryEndpoint endpoint,u32 pb_bus,u32 itd_bus,ChargedAXStorage storage) {
    auto* pb=source.__AXGetPBs();
    Check(pb==storage.address,"original PB alias no longer points to its actual static array");
    Check(sizeof(AXPB)==320 && offsetof(AXPB,mixerCtrl)==12 && offsetof(AXPB,padding)==296,
          "source AXPB mixed-field shape changed");
    std::vector<unsigned char> raw(96*320);
    DSPBackendReadMemory(endpoint,pb_bus,raw.data(),raw.size());
    for(unsigned i=0;i<96;++i) {
        const auto* p=raw.data()+i*320;
        Check((u32(BE16(p))<<16|BE16(p+2))==(i==95?0:pb_bus+(i+1)*320),"PB next address truncates a native pointer");
        Check((u32(BE16(p+4))<<16|BE16(p+6))==pb_bus+i*320,"PB self address lost its physical owner");
        Check((u32(BE16(p+0x46))<<16|BE16(p+0x48))==itd_bus+i*64,"PB ITD address differs from original64-byte stride");
    }
    auto* voice=source.AXAcquireVoice(7,nullptr,0x123456789ULL);
    Check(voice && voice->index==95 && voice->userContext==0x123456789ULL,"original highest free voice/native user context changed");
    AXPBMIX mix{};mix.vL=0x1234;mix.vDeltaL=0x2345;mix.vAuxCL=0x4567;mix.vAuxAS=0x5678;
    source.AXSetVoiceMix(voice,&mix);
    source.AXSetVoiceState(voice,AX_VOICE_RUN);
    AXPBVE ve{0x9876,-321};source.AXSetVoiceVe(voice,&ve);
    voice->sync=AX_PBSYNC_ALL;
    for(unsigned i=0;i<24;++i)voice->pb.padding[i]=static_cast<u8>(i*17+5);
    source.__AXServiceVPB(voice);
    std::array<unsigned char,320> wire{};const auto address=pb_bus+voice->index*320;
    DSPBackendReadMemory(endpoint,address,wire.data(),wire.size());
    Check(BE32(wire.data()+12)==0x04080005,"DSP mixerCtrl was exported as two native-swapped halves");
    Check(BE16(wire.data()+0x14)==0x1234 && BE16(wire.data()+0x16)==0x2345,"DSP native mix fields are not BE16");
    Check(BE16(wire.data()+0x6a)==0x9876 && BE16(wire.data()+0x6c)==u16(-321),"DSP signed volume delta bits changed");
    for(unsigned i=0;i<24;++i)Check(wire[296+i]==static_cast<u8>(i*17+5),"DSP opaque PB padding was byte-swapped");
    // Actual DSP writeback bytes, then the unchanged original sync==0 readback.
    const std::array<unsigned char,2> stopped{0,0},volume{0xab,0xcd};
    const std::array<unsigned char,4> current{0x12,0x34,0x56,0x78};
    DSPBackendWriteMemory(endpoint,address+16,stopped.data(),stopped.size());
    DSPBackendWriteMemory(endpoint,address+0x6a,volume.data(),volume.size());
    DSPBackendWriteMemory(endpoint,address+0x7a,current.data(),current.size());
    voice->sync=0;source.__AXServiceVPB(voice);
    Check(voice->pb.state==0 && voice->pb.ve.currentVolume==0xabcd &&
          voice->pb.addr.currentAddressHi==0x1234 && voice->pb.addr.currentAddressLo==0x5678,
          "original CPU readback did not observe the genuine typed device bytes");
    // A partial DSP store in the high byte of the u32 is independent of host endianness.
    const unsigned char high=0xef;DSPBackendWriteMemory(endpoint,address+12,&high,1);
    Check(pb[voice->index].mixerCtrl==(0xef000000u|(voice->pb.mixerCtrl&0x00ffffffu)),"partial DSP u32 store changed neighboring fields");
    source.AXFreeVoice(voice);
    unsigned free_count{};
    for(auto* next=source.__AXGetStackHead(0);next;next=static_cast<AXVPB*>(next->next)) {
        Check(++free_count<=96,"original free voice chain became cyclic");
    }
    Check(free_count==96,"original source voice release lost a pool entry");
}

void CheckStudio(Source& source,NativeDSPMemoryEndpoint endpoint,u32 studio_bus) {
    source.__AXSPBInit();
    AXPB input{};
    input.dpop.aL=4660;input.dpop.aR=-800;input.dpop.aS=95;
    input.dpop.aAuxAL=1920;input.dpop.aAuxAR=-1920;
    input.dpop.aAuxBL=32767;input.dpop.aAuxBR=-32768;input.dpop.aAuxBS=97;
    input.dpop.aAuxCL=96;input.dpop.aAuxCR=-96;input.dpop.aAuxCS=18;
    input.rmtDpop.aMain0=600;input.rmtDpop.aAux0=-400;
    input.rmtDpop.aMain1=36;input.rmtDpop.aAux1=360;
    input.rmtDpop.aMain2=-18;input.rmtDpop.aAux2=-19;
    input.rmtDpop.aMain3=1;input.rmtDpop.aAux3=-1;
    source.__AXDepopVoice(&input);source.__AXPrintStudio();
    // Independently calculated signed value/delta records in the declared
    // hardware field order, including original sub-frame zeroing/clamps.
    constexpr std::array<s32,20> values{4660,-800,0,1920,-1920,0,32767,-32768,97,96,-96,0,
                                       600,-400,36,360,-18,-19,0,0};
    constexpr std::array<s16,20> deltas{-20,8,0,-20,20,0,-20,20,-1,-1,1,0,
                                        -20,20,-2,-20,1,1,0,0};
    std::array<unsigned char,120> wire{};
    DSPBackendReadMemory(endpoint,studio_bus,wire.data(),wire.size());
    for(unsigned i=0;i<20;++i) {
        Check(BE32(wire.data()+6*i)==static_cast<u32>(values[i]),"packed original Studio s32 field lost its DSP word order");
        Check(BE16(wire.data()+6*i+4)==static_cast<u16>(deltas[i]),"packed original Studio signed delta lost its bits");
    }
    std::array<unsigned char,7> partial{};
    DSPBackendReadMemory(endpoint,studio_bus+5,partial.data(),partial.size());
    Check(std::equal(partial.begin(),partial.end(),wire.begin()+5),"DSP byte read spanning packed field boundaries changed data");
}

void Run(int argc,char** argv) {
    Check(argc==3,"requires the actual six-TU source image and its SHA256 identity");
    Check(std::strlen(argv[2])==64,"source image identity must be a real SHA256");
    SDK sdk;
    const auto directory=std::filesystem::absolute("native-ax-transport-data").string();
    std::filesystem::create_directories(directory);
    AuroraConfig config{};config.appName="Original AX wire transport";
    config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;
    config.windowPosX=config.windowPosY=-1;config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64u*1024u*1024u;
    config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual core SDK window missing");
    sdk.live=true;OSInit();
    Image image(argv[1]);Source source(image.initial);Backings backings;
    const auto commands=source.ChargedAXGetCommandStorage();ChargedAXStorage voices[2],aux[3];
    source.ChargedAXGetVoiceStorage(voices);source.ChargedAXGetAuxStorage(aux);
    const auto compressor=source.ChargedAXGetCompressorStorage();
    ChargedAXStorage studio{source.__AXGetStudio(),sizeof(AXSTUDIO)};
    Check(commands.bytes==256 && voices[0].bytes==96*320 && voices[1].bytes==96*64 &&
          aux[0].bytes==4608 && aux[1].bytes==4608 && aux[2].bytes==3456 && studio.bytes==120,
          "original static storage geometry changed");
    const auto command_bus=backings.Map(image,argv[2],"AXCommandLists",commands,true);
    const auto pb_bus=backings.Map(image,argv[2],"AXPB",voices[0],true);
    const auto itd_bus=backings.Map(image,argv[2],"AXITD",voices[1],true);
    std::array<u32,3> aux_bus{};
    for(unsigned i=0;i<3;++i)aux_bus[i]=backings.Map(image,argv[2],i==0?"AXAuxA":i==1?"AXAuxB":"AXAuxC",aux[i],true);
    const auto compressor_bus=backings.Map(image,argv[2],"AXCompressor",compressor,false);
    const auto studio_bus=backings.Map(image,argv[2],"AXStudio",studio,false);
    const auto endpoint=AttachNativeDSPMEM1();backings.attached=true;
    backings.Pin(commands,true,NativeDSPMemoryEncoding::NativeU16);
    backings.Pin(voices[0],true,NativeDSPMemoryEncoding::AXParameterBlocks);
    backings.Pin(voices[1],true,NativeDSPMemoryEncoding::RawBytes);
    for(auto storage:aux)backings.Pin(storage,true,NativeDSPMemoryEncoding::NativeU32);
    backings.Pin(compressor,false,NativeDSPMemoryEncoding::NativeU16);
    backings.Pin(studio,false,NativeDSPMemoryEncoding::AXStudio);
    DSPBackendValidateMemory(endpoint,pb_bus,96*320,true);
    DSPBackendValidateMemory(endpoint,studio_bus,120,false);
    Throws([&]{DSPBackendValidateMemory(endpoint,studio_bus,120,true);},"side-effect-free device validation accepted readonly Studio writes");
    Throws([&]{DSPBackendValidateMemory(endpoint,pb_bus+96*320-1,2,false);},"device validation accepted a range crossing PB backing");
    Check(image.leases.size()==8,"actual source image leases were not retained by all static owners");
    SDL_UnloadObject(image.initial);image.initial=nullptr;
    Throws([&]{OSNativeReleaseStaticMemory(backings.statics[0]);},"live DSP pin allowed its source image owner to retire");
    const auto game_high=OSGetArenaHi();OSAllocFromArenaLo(32,32);
    Check(OSGetArenaHi()==game_high,"source capture changed actual static reservations");
    source.__AXAllocInit();source.__AXVPBInit();source.__AXSPBInit();
    CheckPB(source,endpoint,pb_bus,itd_bus,voices[0]);
    CheckStudio(source,endpoint,studio_bus);
    auto* surround=static_cast<u8*>(OSAllocFromMEM2ArenaLo(768,32));
    auto* lr=static_cast<u8*>(OSAllocFromMEM2ArenaLo(384,32));
    auto* remote=static_cast<u8*>(OSAllocFromMEM2ArenaLo(4*64,32));
    backings.Pin({surround,768},true,NativeDSPMemoryEncoding::NativeU32);
    backings.Pin({lr,384},true,NativeDSPMemoryEncoding::NativeU16);
    backings.Pin({remote,4*64},true,NativeDSPMemoryEncoding::NativeU16);
    void* remotes[4]{remote,remote+64,remote+128,remote+192};
    Check(OSCachedToPhysical(surround)==0x10000000,"actual MEM2 DSP word differs from canonical Wii physical bank");
    CheckCommands(source,endpoint,command_bus,studio_bus,pb_bus,aux_bus,compressor_bus,surround,lr,remotes);
    std::array<unsigned char,4> wire{0x12,0x34,0x56,0x78};
    DSPBackendWriteMemory(endpoint,OSCachedToPhysical(surround),wire.data(),wire.size());
    u32 native{};std::memcpy(&native,surround,4);Check(native==0x12345678,"actual native32 DSP sample store changed numeric bits");
    DSPBackendWriteMemory(endpoint,OSCachedToPhysical(lr),wire.data(),wire.size());
    u16 left{},right{};std::memcpy(&left,lr,2);std::memcpy(&right,lr+2,2);
    Check(left==0x1234 && right==0x5678,"DSP PCM16 channel cells were reinterpreted as native32 values");
    // Source ITD storage remains an opaque byte domain, with no numeric swap.
    DSPBackendWriteMemory(endpoint,itd_bus,wire.data(),wire.size());
    Check(std::memcmp(voices[1].address,wire.data(),wire.size())==0,"raw ITD device bytes changed with native typed transport");
    Throws([&]{PinNativeDSPMemory(surround+766,2,true,NativeDSPMemoryEncoding::NativeU32);},"incomplete native32 transport record succeeded");
    Throws([&]{PinNativeDSPMemory(surround,4,true,static_cast<NativeDSPMemoryEncoding>(99));},"unknown native encoding gained a success fallback");
    Throws([&]{DSPBackendWriteMemory(endpoint,compressor_bus,wire.data(),1);},"DSP wrote read-only original compressor data");
    Throws([&]{DSPBackendReadMemory(endpoint,pb_bus+96*320-1,wire.data(),2);},"typed DSP transfer crossed actual PB extent");
    std::exception_ptr failure;
    std::thread worker([&]{
        try {
            std::array<unsigned char,4> actual{};DSPBackendReadMemory(endpoint,OSCachedToPhysical(surround),actual.data(),actual.size());
            if(actual!=wire)throw std::runtime_error("device-thread typed read differs from physical wire bytes");
        } catch(...) {failure=std::current_exception();}
    });worker.join();if(failure)std::rethrow_exception(failure);
    ++checks;
    const auto released=backings.pins.back();ReleaseNativeDSPMemory(released);backings.pins.pop_back();
    Throws([&]{DSPBackendValidateMemory(endpoint,OSCachedToPhysical(remote),1,true);},"device validation retained a released source pin");
    Throws([&]{ChargedDSPTaskMemoryWord(remote,1,0);},"source pointer gained a bus word after its actual pin retired");
    for(auto it=backings.pins.rbegin();it!=backings.pins.rend();++it)ReleaseNativeDSPMemory(*it);
    backings.pins.clear();DetachNativeDSPMEM1();backings.attached=false;
    Throws([&]{DSPBackendReadMemory(endpoint,pb_bus,wire.data(),1);},"retired DSP endpoint retained source access");
    for(auto it=backings.statics.rbegin();it!=backings.statics.rend();++it)OSNativeReleaseStaticMemory(*it);
    backings.statics.clear();Check(image.leases.empty(),"source image leases survived genuine pin/owner retirement");
    std::cout<<"Original AX typed transport: "<<checks<<" checks;96 PB links,8 command lists; AXOut/DSP/FX initialization unexecuted\n";
}
}
int main(int argc,char** argv) {
    try {Run(argc,argv);return 0;}
    catch(const std::exception& error) {std::cerr<<"Original AX transport: "<<error.what()<<'\n';return 1;}
}
