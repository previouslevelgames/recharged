#include "platform/ax_adpcm_samples.h"
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
#include <revolution/sp.h>
}
#include <array>
#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace mscharged::platform;
unsigned checks{},raw_samples{};
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
    AX_FUNCTION(__AXSyncPBs) AX_FUNCTION(AXSetVoiceLoop) AX_FUNCTION(AXSetVoiceLoopAddr)
    AX_FUNCTION(AXSetVoiceEndAddr) AX_FUNCTION(AXSetVoiceCurrentAddr) AX_FUNCTION(AXSetVoiceAdpcm)
    AX_FUNCTION(__AXServiceVPB) AX_FUNCTION(__AXDepopVoice) AX_FUNCTION(__AXPrintStudio)
    AX_FUNCTION(SPInitSoundTable) AX_FUNCTION(SPGetSoundEntry) AX_FUNCTION(SPPrepareSound)
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
        AX_FUNCTION(__AXSyncPBs) AX_FUNCTION(AXSetVoiceLoop) AX_FUNCTION(AXSetVoiceLoopAddr)
    AX_FUNCTION(AXSetVoiceEndAddr) AX_FUNCTION(AXSetVoiceCurrentAddr) AX_FUNCTION(AXSetVoiceAdpcm)
    AX_FUNCTION(__AXServiceVPB) AX_FUNCTION(__AXDepopVoice) AX_FUNCTION(__AXPrintStudio)
        AX_FUNCTION(SPInitSoundTable) AX_FUNCTION(SPGetSoundEntry) AX_FUNCTION(SPPrepareSound)
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
        if (!initial) throw std::runtime_error(std::string("actual seven-TU source image could not load: ")+SDL_GetError());
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

std::vector<unsigned char> File(const char* path) {
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("owned codec input absent");
    return {std::istreambuf_iterator<char>(file),{}};
}
u32 LE32(const unsigned char* p) {return u32(p[0])|u32(p[1])<<8|u32(p[2])<<16|u32(p[3])<<24;}
s16 LE16(const unsigned char* p) {const u16 bits=u16(p[0])|u16(p[1])<<8;s16 x;std::memcpy(&x,&bits,2);return x;}
void Run(int argc,char** argv) {
    Check(argc==3 || argc==6,"requires seven-whole-TU AX/SP image and SHA256, with optional owned resource/sample/PCM oracle");
    const bool owned=argc==6;
    const auto resource=owned?File(argv[3]):std::vector<unsigned char>{};
    const auto sample=owned?File(argv[4]):std::vector<unsigned char>{};
    const auto golden=owned?File(argv[5]):std::vector<unsigned char>{};
    if(owned)Check(resource.size()==1248 && sample.size()==303445 && golden.size()>=8 && std::memcmp(golden.data(),"AXRAW001",8)==0,"owned codec records differ");
    SDK sdk;
    const auto directory=std::filesystem::absolute("native-ax-adpcm-data").string();std::filesystem::create_directories(directory);
    AuroraConfig config{};config.appName="Native AX raw ADPCM";config.userPath=config.cachePath=directory.c_str();config.resourcesPath=".";
    config.desiredBackend=BACKEND_NULL;config.windowWidth=320;config.windowHeight=240;config.windowPosX=config.windowPosY=-1;
    config.mem1Size=MEM1_DEFAULT_SIZE;config.mem2Size=64u*1024u*1024u;config.logLevel=LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window!=nullptr,"actual single CPU SDK initialization failed");sdk.live=true;OSInit();
    Image image(argv[1]);Source source(image.initial);Backings backing;
    ChargedAXStorage voice[2];source.ChargedAXGetVoiceStorage(voice);
    const auto pb=backing.Map(image,argv[2],"ADPCM-PBs",voice[0],true);
    backing.Map(image,argv[2],"ADPCM-ITD",voice[1],true);
    const auto endpoint=AttachNativeDSPMEM1();backing.attached=true;
    backing.Pin(voice[0],true,NativeDSPMemoryEncoding::AXParameterBlocks);
    backing.Pin(voice[1],true,NativeDSPMemoryEncoding::RawBytes);
    source.__AXAllocInit();source.__AXVPBInit();
    if(owned) {
    auto* encoded=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(sample.size(),32));std::memcpy(encoded,sample.data(),sample.size());
    backing.Pin({encoded,static_cast<u32>(sample.size())},false,NativeDSPMemoryEncoding::RawBytes);
    const auto physical=OSCachedToPhysical(encoded);
    // Controlled metadata admission in this codec-only fixture;307 independently
    // qualifies genuine original loader/NL ownership. Original whole SP functions
    // below produce the actual voice descriptors, not a native PB constructor.
    const auto bytes=offsetof(SPSoundTable,sound)+5*sizeof(SPSoundEntry)+5*sizeof(SPADPCM);
    auto memory=std::make_unique<std::max_align_t[]>((bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
    auto* table=reinterpret_cast<SPSoundTable*>(memory.get());table->entries=5;
    auto* coefficients=reinterpret_cast<SPADPCM*>(&table->sound[5]);
    for(unsigned i=0;i<5;++i) {
        const auto* r=resource.data()+700+i*28;auto& sound=table->sound[i];
        sound.type=BE32(r);sound.sampleRate=BE32(r+4);sound.loopAddr=BE32(r+8);sound.loopEndAddr=BE32(r+12);
        sound.endAddr=BE32(r+16);sound.currentAddr=BE32(r+20);sound.adpcm=nullptr;
        for(unsigned j=0;j<23;++j) {
            const auto value=BE16(resource.data()+840+i*46+j*2);
            std::memcpy(reinterpret_cast<unsigned char*>(&coefficients[i])+j*2,&value,2);
        }
    }
    source.SPInitSoundTable(table,physical+0x80000000u,0);
    std::size_t oracle_at=8;
    for(unsigned i=0;i<5;++i) {
        auto* sound=source.SPGetSoundEntry(table,i);auto* native=source.AXAcquireVoice(7,nullptr,0);
        Check(native && sound && sound->adpcm==coefficients+i,"original SP entry/source voice ownership differs");
        source.SPPrepareSound(sound,native,sound->sampleRate);source.AXSetVoiceState(native,AX_VOICE_RUN);
        source.__AXServiceVPB(native);
        const auto address=pb+native->index*320;const auto initial=ReadNativeAXADPCMState(endpoint,address);
        Check(oracle_at+4<=golden.size(),"owned PCM oracle count exceeds actual input");
        const auto count=LE32(golden.data()+oracle_at);oracle_at+=4;
        Check(std::size_t(count)<=((golden.size()-oracle_at)/2),"owned PCM oracle samples exceed actual input");
        const auto expected=golden.data()+oracle_at;oracle_at+=std::size_t(count)*2;
        Check(initial.running && initial.src_select==0 && initial.coefficient_select==0 && initial.current_nibble==sound->currentAddr &&
              initial.end_nibble==sound->endAddr && initial.ratio==(sound->sampleRate==32000?65536u:90316u),
              "actual original SP/AX descriptor/SRC choice changed");
        Throws([&]{RequireNativeAXADPCMFrameProcessing(initial);},"default source4tap/ratiounity got a fake direct frame");
        std::array<unsigned char,320> unchanged{};DSPBackendReadMemory(endpoint,address,unchanged.data(),unchanged.size());
        auto state=initial;std::size_t decoded=0;NativeAXRawADPCMBlock block;
        while(state.running) {
            block=DecodeNativeAXRawADPCM(endpoint,state,96);
            Check(block.samples_decoded>0 && decoded+block.samples_decoded<=count,"raw accelerator produced an invalid decoded count");
            bool equal=true;
            for(unsigned j=0;j<block.samples_decoded;++j)equal&=block.samples[j]==LE16(expected+(decoded+j)*2);
            Check(equal,"raw hardware ADPCM differs from independent whole-owned-sample PCM oracle");
            decoded+=block.samples_decoded;state=block.next;
        }
        raw_samples+=decoded;
        Check(decoded==count && block.end_reached && state.current_nibble==initial.loop_nibble && !state.running && block.loops==0,
              "inclusive authored end/one-shot source loop address behavior changed");
        Check(state.history1==LE16(expected+(count-1)*2) && state.history2==LE16(expected+(count-2)*2),
              "actual ADPCM terminal predictor histories differ from owned PCM");
        std::array<unsigned char,320> after{};DSPBackendReadMemory(endpoint,address,after.data(),after.size());
        Check(after==unchanged,"raw codec gate wrote source PB/SRC state without full frame processing");
        const auto stopped=DecodeNativeAXRawADPCM(endpoint,state,96);Check(stopped.samples_decoded==0,"stopped raw device state decoded more samples");
        source.AXSetVoiceState(native,AX_VOICE_STOP);source.__AXServiceVPB(native);source.AXFreeVoice(native);
    }
    Check(oracle_at==golden.size(),"independent PCM oracle suffix was unconsumed");
    Check(std::memcmp(encoded,sample.data(),sample.size())==0,"read-only decoder changed original encoded bytes");
    }
    // Synthetic hardware edge fixtures use the same real checked byte bus, no
    // synthetic callback/readiness. Zero coefficients make residual PCM exact.
    // 48 owned bytes, 32 pinned: the tail stands for the memory that follows a
    // sample allocation, which the accelerator may read for one ADPCM frame.
    auto* bytes_for_edges=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(48,32));
    for(unsigned i=32;i<48;++i)bytes_for_edges[i]=static_cast<unsigned char>(0xa0+i);
    const std::array<unsigned char,32> synthetic{0,0x17,0x8f,0x20,0x04,0x00,0x00,0x00,0,0,0,0,0,0,0,0,
                                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    std::memcpy(bytes_for_edges,synthetic.data(),32);backing.Pin({bytes_for_edges,32},true,NativeDSPMemoryEncoding::RawBytes);
    const auto base=OSCachedToPhysical(bytes_for_edges)*2;
    // Portable actual producer gate: genuine SP methods prepare a generated
    // ADPCM hardware sample; srcSelect/coefSelect retain original zero defaults.
    alignas(SPSoundTable) std::array<unsigned char,
        offsetof(SPSoundTable,sound)+sizeof(SPSoundEntry)+sizeof(SPADPCM)> fixture_table{};
    auto* table=reinterpret_cast<SPSoundTable*>(fixture_table.data());
    table->entries=1;table->sound[0].type=0;table->sound[0].sampleRate=32000;
    table->sound[0].currentAddr=2;table->sound[0].endAddr=5;
    source.SPInitSoundTable(table,OSCachedToPhysical(bytes_for_edges)+0x80000000u,0);
    auto* voice_fixture=source.AXAcquireVoice(7,nullptr,0);
    Check(voice_fixture!=nullptr,"original synthetic SP voice acquire failed");
    source.SPPrepareSound(source.SPGetSoundEntry(table,0),voice_fixture,32000);
    source.AXSetVoiceState(voice_fixture,AX_VOICE_RUN);source.__AXServiceVPB(voice_fixture);
    const auto address=pb+voice_fixture->index*320;
    const auto source_state=ReadNativeAXADPCMState(endpoint,address);
    const auto source_block=DecodeNativeAXRawADPCM(endpoint,source_state,96);
    constexpr std::array<s16,4> source_expected{1,7,-8,-1};
    Check(source_block.samples_decoded==4 && std::equal(source_expected.begin(),source_expected.end(),source_block.samples.begin()),
          "actual original SP/AX descriptor produced different synthetic PCM");
    Check(source_state.src_select==0 && source_state.coefficient_select==0 && source_state.ratio==65536,
          "portable source producer changed default4tap selection");
    Throws([&]{RequireNativeAXADPCMFrameProcessing(source_state);},"portable ratio1 source4tap became a direct voice frame");
    source.AXSetVoiceState(voice_fixture,AX_VOICE_STOP);source.__AXSyncPBs(0);
    // Genuine source setters drive each partial sync path. Decoder field input
    // stays untouched; these checks qualify native halfword word-copy ABI only.
    source.AXSetVoiceLoopAddr(voice_fixture,0x01234567);
    source.AXSetVoiceEndAddr(voice_fixture,0x089abcde);
    source.AXSetVoiceCurrentAddr(voice_fixture,0x00112233);
    source.__AXSyncPBs(0);
    auto partial=ReadNativeAXADPCMState(endpoint,address);
    Check(partial.loop_nibble==0x01234567 && partial.end_nibble==0x089abcde && partial.current_nibble==0x00112233,
          "partial original source word sync changed halfword order");
    const std::array<unsigned char,4> device_address{0x05,0x67,0x89,0xab};
    DSPBackendWriteMemory(endpoint,address+0x7a,device_address.data(),device_address.size());
    source.AXSetVoiceLoop(voice_fixture,1);source.__AXSyncPBs(0);
    Check(voice_fixture->pb.addr.currentAddressHi==0x0567 && voice_fixture->pb.addr.currentAddressLo==0x89ab,
          "original source partial sync failed to receive device current-address words");
    alignas(4) std::array<unsigned char,sizeof(AXPBADPCM)+2> halfword_aligned{};
    auto* adpcm=reinterpret_cast<AXPBADPCM*>(halfword_aligned.data()+2);
    for(unsigned i=0;i<sizeof(AXPBADPCM)/2;++i) {
        const u16 value=0x1234+i*0x321;std::memcpy(halfword_aligned.data()+2+i*2,&value,2);
    }
    source.AXSetVoiceAdpcm(voice_fixture,adpcm);source.__AXSyncPBs(0);
    std::array<unsigned char,sizeof(AXPBADPCM)> adpcm_wire{};
    DSPBackendReadMemory(endpoint,address+0x7e,adpcm_wire.data(),adpcm_wire.size());
    bool words_equal=true;
    for(unsigned i=0;i<sizeof(AXPBADPCM)/2;++i)words_equal&=BE16(adpcm_wire.data()+i*2)==u16(0x1234+i*0x321);
    Check(words_equal,"original unrolled ADPCM setter/sync changed native halfword bytes");
    source.AXFreeVoice(voice_fixture);
    NativeAXADPCMState state{};state.current_nibble=base+2;state.end_nibble=base+5;state.loop_nibble=base+2;
    state.running=true;state.predictor_scale=0;state.loop_predictor_scale=0;state.loop_flag=1;
    auto looped=DecodeNativeAXRawADPCM(endpoint,state,10);
    constexpr std::array<s16,10> expected{1,7,-8,-1,1,7,-8,-1,1,7};
    Check(std::equal(expected.begin(),expected.end(),looped.samples.begin()) && looped.loops==2 && looped.next.current_nibble==base+4,
          "real checked nibble high/low/loop order differs from independent four-sample cycle");
    state.coefficients[0]={2048,0};state.history1=10;state.loop_history1=100;
    const auto normal=DecodeNativeAXRawADPCM(endpoint,state,6);
    constexpr std::array<s16,6> normal_expected{11,18,10,9,101,108};
    Check(std::equal(normal_expected.begin(),normal_expected.end(),normal.samples.begin()),"normal loop did not restore authored loop history");
    state.voice_type=1;const auto stream=DecodeNativeAXRawADPCM(endpoint,state,6);
    constexpr std::array<s16,6> stream_expected{11,18,10,9,10,17};
    Check(std::equal(stream_expected.begin(),stream_expected.end(),stream.samples.begin()),"stream loop did not preserve current histories");
    state.voice_type=0;state.loop_flag=0;state.end_nibble=base+15;state.coefficients={};
    const auto boundary=DecodeNativeAXRawADPCM(endpoint,state,14);
    Check(boundary.samples_decoded==14 && boundary.end_reached && boundary.next.current_nibble==base+2,
          "last payload/header prefetch/inclusive end boundary changed");
    // The terminal prefetch reads the genuine next header: a one-shot voice
    // writes it back, a looping voice replaces it by its loop pred/scale.
    bytes_for_edges[8]=0x35;
    const auto one_shot_header=DecodeNativeAXRawADPCM(endpoint,state,14);
    Check(one_shot_header.end_reached && !one_shot_header.next.running && one_shot_header.next.predictor_scale==0x35,
          "one-shot terminal header prefetch did not write back the genuine next header");
    state.loop_flag=1;state.loop_predictor_scale=0x22;
    const auto looped_header=DecodeNativeAXRawADPCM(endpoint,state,14);
    Check(looped_header.end_reached && looped_header.next.running && looped_header.loops==1 &&
              looped_header.next.predictor_scale==0x22,"looping terminal prefetch did not yield the loop pred/scale");
    state.loop_flag=0;state.loop_predictor_scale=0;bytes_for_edges[8]=0;
    state.end_nibble=base+16;Throws([&]{DecodeNativeAXRawADPCM(endpoint,state,14);},"unsupported header-nibble end silently became ordinary decoding");
    state.end_nibble=base+31;state.current_nibble=base+30;state.loop_nibble=base+30;
    const auto before=state;
    // Past the allocation the accelerator reads the memory that follows it: a
    // terminal prefetch, and a retail end address one frame beyond (stream end).
    state.current_nibble=base+62;state.end_nibble=base+63;state.loop_nibble=base+62;state.predictor_scale=0x11;
    const auto terminal=DecodeNativeAXRawADPCM(endpoint,state,2);
    Check(terminal.samples_decoded==2 && terminal.end_reached && !terminal.next.running &&
              terminal.next.predictor_scale==(0xa0+32)%128 && terminal.next.current_nibble==base+62,
          "terminal prefetch past the allocation did not read the following memory");
    state.end_nibble=base+66;
    const auto overrun=DecodeNativeAXRawADPCM(endpoint,state,4);
    Check(overrun.samples_decoded==3 && overrun.end_reached && !overrun.next.running &&
              overrun.next.predictor_scale==(0xa0+32)%128,"stream end one frame past its buffer was not decoded");
    // Beyond one ADPCM frame the read stays a genuine pin failure.
    state.end_nibble=base+90;
    Throws([&]{DecodeNativeAXRawADPCM(endpoint,state,24);},"unrepresented header prefetch supplied a fake padded byte");
    state=before;state.end_nibble=base+31;
    Throws([&]{DecodeNativeAXRawADPCM({endpoint.generation+1},state,2);},"stale byte endpoint was decoded");
    state.predictor_scale=15;state.current_nibble=base+2;state.end_nibble=base+5;state.loop_nibble=base+2;
    const auto saturated=DecodeNativeAXRawADPCM(endpoint,state,4);
    constexpr std::array<s16,4> clip{32767,32767,-32768,-32768};
    Check(std::equal(clip.begin(),clip.end(),saturated.samples.begin()),"signed residual/scale15 saturation changed");
    // Only a looping voice decodes from its loop address (firmware 0F61..0F71).
    state.loop_flag=1;
    state.loop_nibble=base+6;Throws([&]{DecodeNativeAXRawADPCM(endpoint,state,1);},"loop target beyond authored inclusive end was accepted");
    state.loop_nibble=base+16;Throws([&]{DecodeNativeAXRawADPCM(endpoint,state,1);},"looping header-nibble target became ordinary decoding");
    // A one-shot voice stops at its end exception; its silence-buffer loop
    // address at a frame header beyond the sample is only written back.
    state.loop_flag=0;
    const auto one_shot=DecodeNativeAXRawADPCM(endpoint,state,6);
    Check(one_shot.samples_decoded==4 && one_shot.end_reached && !one_shot.next.running && one_shot.loops==0 &&
              one_shot.next.current_nibble==base+16 && one_shot.next.predictor_scale==15 &&
              std::equal(clip.begin(),clip.end(),one_shot.samples.begin()),
          "one-shot end did not stop at its unread header-aligned loop address");
    state.loop_nibble=base+2;
    Throws([&]{DecodeNativeAXRawADPCM(endpoint,state,97);},"raw block crossed original96-sample frame capacity");
    std::cout<<"Native AX raw DSP ADPCM: "<<checks<<" checks; "<<raw_samples
             <<" owned raw samples; original four-tap SRC/frame output/bootstrap remain unavailable\n";
}
}
int main(int argc,char** argv) {
    try{Run(argc,argv);return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
