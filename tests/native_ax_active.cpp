#include "platform/ax_bootstrap_device.h"
#include "platform/ax_active_voice.h"
#include "platform/ax_command_service.h"
#include "platform/ai.h"
#include "platform/ax_storage_abi.h"
#include "platform/dsp_control_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <dolphin/ai.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_timer.h>
extern "C" {
#include <revolution/ax.h>
#include <revolution/dsp.h>
#include <revolution/sp.h>
#include <revolution/mix.h>
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
#include <vector>

namespace {
using namespace mscharged::platform;
unsigned checks{};
unsigned source_frame_callbacks{}, source_aux_callbacks{};
void ObserveFrame() { ++source_frame_callbacks; }
void ProduceAux(void* samples, void*) {
    // Synthetic negative contributor through the real source callback slot.
    static_cast<s32*>(samples)[0] = 1;
    ++source_aux_callbacks;
}
void Check(bool value, const char* text) {
    ++checks;
    if (!value) throw std::runtime_error(text);
}
template<class F> void Throws(F operation, const char* text) {
    bool failed{};
    try { operation(); } catch (const std::exception&) { failed = true; }
    Check(failed, text);
}
template<class F> F Load(SDL_SharedObject* image, const char* name) {
    auto* result = SDL_LoadFunction(image, name);
    if (!result) throw std::runtime_error(std::string(name) + ": " + SDL_GetError());
    return reinterpret_cast<F>(result);
}
struct Lease {
    std::string path;
    std::vector<SDL_SharedObject*> images;
    static BOOL Retain(void* context) noexcept {
        auto& lease = *static_cast<Lease*>(context);
        auto* image = SDL_LoadObject(lease.path.c_str());
        if (!image) return FALSE;
        try { lease.images.push_back(image); }
        catch (...) { SDL_UnloadObject(image); return FALSE; }
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& lease = *static_cast<Lease*>(context);
        auto* image = lease.images.back();
        lease.images.pop_back();
        SDL_UnloadObject(image);
    }
};
struct Span {
    const char* name;
    ChargedAXStorage storage;
    NativeDSPMemoryEncoding encoding;
    bool writable;
    OSNativeStaticMemory mapping{};
    NativeDSPMemoryPin pin{};
};
std::vector<unsigned char> File(const char* path) {
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("conformance fixture missing");
    return {std::istreambuf_iterator<char>(file),{}};
}
u16 BE16(const unsigned char* p) { return (u16(p[0])<<8)|p[1]; }
u32 BE32(const unsigned char* p) { return (u32(BE16(p))<<16)|BE16(p+2); }
u16 LE16(const unsigned char* p) { return u16(p[0])|(u16(p[1])<<8); }
u32 LE32(const unsigned char* p) { return u32(LE16(p))|(u32(LE16(p+2))<<16); }
s16 Signed(u16 bits) {s16 value;std::memcpy(&value,&bits,2);return value;}
void Run(int argc, char** argv) {
    Check(argc==6 || argc==10, "need source image/identity, synthetic SRC/DROM/mix oracle and optional owned Splash leaves/oracle");
    const auto data = std::filesystem::absolute("sdk-data").string();
    std::filesystem::create_directories(data);
    AuroraConfig config{};
    config.appName = "Original AX active voice conformance";
    config.userPath = config.cachePath = data.c_str();
    config.resourcesPath = ".";
    config.desiredBackend = BACKEND_NULL;
    config.windowWidth = 320;
    config.windowHeight = 240;
    config.windowPosX = config.windowPosY = -1;
    config.mem1Size = MEM1_DEFAULT_SIZE;
    config.mem2Size = 64u * 1024u * 1024u;
    config.logLevel = LOG_WARNING;
    Check(aurora_initialize(argc, argv, &config).window != nullptr,
          "sole actual SDK window missing");
    OSInit();
    InitializeNativeInterruptController();
    const auto memory = AttachNativeDSPMEM1();
    const auto mail = AttachNativeDSPMailboxes();
    const auto control = AttachNativeDSPControl(mail);
    Lease lease{std::filesystem::absolute(argv[1]).string(), {}};
    auto* image = SDL_LoadObject(lease.path.c_str());
    if (!image) throw std::runtime_error(std::string("whole original AX/DSP module: ") + SDL_GetError());
    Check(image != nullptr, "whole original AX/DSP module failed to load");
    auto output = Load<decltype(&ChargedAXGetOutputStorage)>(image, "ChargedAXGetOutputStorage");
    auto task_storage = Load<decltype(&ChargedAXGetTaskStorage)>(image, "ChargedAXGetTaskStorage");
    auto command = Load<decltype(&ChargedAXGetCommandStorage)>(image, "ChargedAXGetCommandStorage");
    auto voices = Load<decltype(&ChargedAXGetVoiceStorage)>(image, "ChargedAXGetVoiceStorage");
    auto aux = Load<decltype(&ChargedAXGetAuxStorage)>(image, "ChargedAXGetAuxStorage");
    auto compressor = Load<decltype(&ChargedAXGetCompressorStorage)>(image, "ChargedAXGetCompressorStorage");
    auto studio = Load<decltype(&__AXGetStudio)>(image, "__AXGetStudio");
    ChargedAXStorage outputs[5]{}, voice_storage[2]{}, auxiliary[3]{};
    output(outputs); voices(voice_storage); aux(auxiliary);
    std::array<Span, 13> spans{{
        {"AXCommandLists", command(), NativeDSPMemoryEncoding::NativeU16, true},
        {"AXPB", voice_storage[0], NativeDSPMemoryEncoding::AXParameterBlocks, true},
        {"AXITD", voice_storage[1], NativeDSPMemoryEncoding::RawBytes, true},
        {"AXAuxA", auxiliary[0], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXAuxB", auxiliary[1], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXAuxC", auxiliary[2], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXCompressor", compressor(), NativeDSPMemoryEncoding::NativeU16, false},
        {"AXStudio", {studio(), sizeof(AXSTUDIO)}, NativeDSPMemoryEncoding::AXStudio, false},
        {"AXStereoPCM16", outputs[0], NativeDSPMemoryEncoding::NativeU16, true},
        {"AXSurround32", outputs[1], NativeDSPMemoryEncoding::NativeU32, true},
        {"AXRemotePCM16", outputs[2], NativeDSPMemoryEncoding::NativeU16, true},
        {"AXDramContext", outputs[3], NativeDSPMemoryEncoding::RawBytes, true},
        {"AXFirmware", outputs[4], NativeDSPMemoryEncoding::RawBytes, false}
    }};
    const std::array<std::uint32_t, 13> extents{{256,30720,6144,4608,4608,3456,4032,120,1152,768,1440,64,8192}};
    for (std::size_t i = 0; i < spans.size(); ++i) {
        auto& span = spans[i];
        Check(span.storage.address && span.storage.bytes == extents[i],
              "actual source descriptor geometry differs");
        OSNativeStaticMemoryOwner owner{argv[2], span.name, span.storage.address,
            span.storage.bytes, span.writable, &lease, Lease::Retain, Lease::Release};
        span.mapping = OSNativeRegisterStaticMemory(&owner);
        span.pin = PinNativeDSPMemory(span.storage.address, span.storage.bytes,
                                      span.writable, span.encoding);
        DSPBackendValidateMemory(memory, span.mapping.physical_address,
                                 span.storage.bytes, span.writable);
    }
    const auto stored_task = task_storage();
    Check(stored_task.bytes == sizeof(DSPTask), "actual native CPU task extent differs");
    auto* task = static_cast<DSPTask*>(stored_task.address);
    Check(task && !task->initCallback && !task->flags && !task->state,
          "source private AX task was initialized before actual call");
    auto actual_current = Load<DSPTask**>(image, "__DSP_curr_task");
    auto is_dsp_init = Load<decltype(&DSPCheckInit)>(image, "DSPCheckInit");
    auto is_ax_init = Load<decltype(&AXIsInit)>(image, "AXIsInit");
    auto source_ax_init = Load<decltype(&AXInit)>(image, "AXInit");
    auto source_frame = Load<decltype(&__AXOutNewFrame)>(image, "__AXOutNewFrame");
    auto register_callback = Load<decltype(&AXRegisterCallback)>(image, "AXRegisterCallback");
    auto source_handler = Load<__OSInterruptHandler>(image, "__DSPHandler");
    Check(!is_dsp_init() && !is_ax_init() && !*actual_current,
          "cold source init flags or task owner differs");
    NativeAXBootstrapDevice device(memory, mail, control, spans.back().mapping.physical_address,
                                    NativeAXFrameMode::StoppedVoices);
    // Real AI precedes original AXInit exactly as the source backend requests.
    // No game backend/factory or source flags are replaced by this leaf gate.
    AIInit(nullptr);
    source_ax_init();

    Check(is_ax_init() && is_dsp_init(),"actual whole AXInit failed");
    AIStopDMA(); ServiceNativeAI();
    auto mix_init=Load<decltype(&MIXInit)>(image,"MIXInit");
    auto mix_channel=Load<decltype(&MIXInitChannel)>(image,"MIXInitChannel");
    auto acquire=Load<decltype(&AXAcquireVoice)>(image,"AXAcquireVoice");
    auto release=Load<decltype(&AXFreeVoice)>(image,"AXFreeVoice");
    auto set_state=Load<decltype(&AXSetVoiceState)>(image,"AXSetVoiceState");
    auto set_src=Load<decltype(&AXSetVoiceSrc)>(image,"AXSetVoiceSrc");
    auto set_src_type=Load<decltype(&AXSetVoiceSrcType)>(image,"AXSetVoiceSrcType");
    auto set_ve=Load<decltype(&AXSetVoiceVe)>(image,"AXSetVoiceVe");
    auto set_mix=Load<decltype(&AXSetVoiceMix)>(image,"AXSetVoiceMix");
    auto sync=Load<decltype(&__AXSyncPBs)>(image,"__AXSyncPBs");
    auto service_vpb=Load<decltype(&__AXServiceVPB)>(image,"__AXServiceVPB");
    auto sp_init=Load<decltype(&SPInitSoundTable)>(image,"SPInitSoundTable");
    auto sp_get=Load<decltype(&SPGetSoundEntry)>(image,"SPGetSoundEntry");
    auto sp_prepare=Load<decltype(&SPPrepareSound)>(image,"SPPrepareSound");
    auto set_lpf=Load<decltype(&AXSetVoiceLpf)>(image,"AXSetVoiceLpf");
    auto set_remote=Load<decltype(&AXSetVoiceRmtOn)>(image,"AXSetVoiceRmtOn");
    auto set_loop=Load<decltype(&AXSetVoiceLoop)>(image,"AXSetVoiceLoop");
    auto set_end=Load<decltype(&AXSetVoiceEndAddr)>(image,"AXSetVoiceEndAddr");
    auto set_current=Load<decltype(&AXSetVoiceCurrentAddr)>(image,"AXSetVoiceCurrentAddr");
    mix_init();
    const auto oracle=File(argv[3]), drom_bytes=File(argv[4]), mix_oracle=File(argv[5]);
    Check(oracle.size()>12 && std::memcmp(oracle.data(),"AXSRC001",8)==0 && drom_bytes.size()==4096,
          "synthetic fixture format differs");
    Check(LE32(oracle.data()+8)==15,"selected source FIR cases differ");
    Check(mix_oracle.size()==12+15*(194+12*388) && std::memcmp(mix_oracle.data(),"AXMIX001",8)==0,
          "selected owned envelope/mixer oracle extent differs");
    auto* drom=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(4096,32));
    std::memcpy(drom,drom_bytes.data(),4096);
    const auto drom_pin=PinNativeDSPMemory(drom,4096,false,NativeDSPMemoryEncoding::RawBytes);
    NativeAXSuppliedCoefficientROM coefficients;
    Throws([&]{coefficients.Row(0,0);},"missing authentic bank received guessed coefficients");
    coefficients.Load(memory,OSCachedToPhysical(drom));
    Throws([&]{coefficients.Row(3,0);},"unknown bank substituted a qualified bank");
    auto* sample=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(256,32));
    std::memset(sample,0,256);
    // Explicit synthetic DSP ADPCM fixture: predictor0 coefficients zero,
    // scale12,384 signed4-bit residuals. No original cue chooses this sample.
    for(unsigned i=0;i<28;++i)sample[i*8]=12;
    for(unsigned i=0;i<384;++i) {
        const unsigned nibble=(i/14)*16+2+i%14;
        const unsigned bits=(((i*7+3)%16)-8)&15;
        if(nibble&1)sample[nibble/2]|=bits;
        else sample[nibble/2]|=bits<<4;
    }
    const auto sample_pin=PinNativeDSPMemory(sample,256,false,NativeDSPMemoryEncoding::RawBytes);
    const auto sample_physical=OSCachedToPhysical(sample);
    alignas(SPSoundTable) std::array<unsigned char,
        offsetof(SPSoundTable,sound)+sizeof(SPSoundEntry)+sizeof(SPADPCM)> table_storage{};
    auto* table=reinterpret_cast<SPSoundTable*>(table_storage.data());table->entries=1;
    table->sound[0].sampleRate=32000;table->sound[0].type=0;
    table->sound[0].currentAddr=2;table->sound[0].endAddr=439;
    auto* codec=reinterpret_cast<SPADPCM*>(table->sound+1);codec->adpcm.pred_scale=12;
    sp_init(table,sample_physical+0x80000000u,0);
    std::size_t at=12;unsigned frames=0;
    const auto pb_bus=spans[1].mapping.physical_address;
    for(unsigned index=0;index<15;++index) {
        const auto* c=oracle.data()+at;at+=16+768+192+12;
        Check(at<=oracle.size(),"selected instruction oracle extent differs");
        auto* voice=acquire(15,nullptr,0);
        Check(voice!=nullptr,"original source voice allocation failed");
        sp_prepare(sp_get(table,0),voice,32000);
        mix_channel(voice,0,0,-960,-960,-960,64,127,0);
        if(index==0)
            Check(voice->pb.srcSelect==0 && voice->pb.coefSelect==0 && voice->pb.src.ratioHi==1,
                  "actual fresh SP/MIX source changed4tap selection");
        // Original default/reset and SP preparation deliberately retain SRC
        // selection when a voice is reused. Only the first allocation above
        // proves cold selection; subsequent synthetic cases use real setters.
        const auto ratio=LE32(c);const auto initial_fraction=LE16(c+4);const auto bank=LE16(c+6);
        AXPBSRC src{};src.ratioHi=ratio>>16;src.ratioLo=ratio;src.currentAddressFrac=initial_fraction;
        for(unsigned i=0;i<4;++i)src.last_samples[i]=LE16(c+8+i*2);
        set_src(voice,&src);set_src_type(voice,AX_SRC_TYPE_4TAP_8K+bank);
        const std::array<AXPBVE,4> envelopes{{{0xffff,17},{1,-17},{0x8000,0},{0x7400,3}}};
        auto envelope=envelopes[index%4];set_ve(voice,&envelope);
        auto mixed=voice->pb.mix;
        // Exercise real source setter/ramp flags for every main bus in this
        // hardware conformance case; this is not a game event/voice recipe.
        auto* fields=reinterpret_cast<u16*>(&mixed);
        const u16 initial_mix=index%4==0?0xffff:index%4==1?1:10000;
        for(unsigned bus=0;bus<12;++bus){fields[bus*2]=u16(initial_mix+bus*503);fields[bus*2+1]=s16(bus%2?-7:11);}
        set_mix(voice,&mixed);set_state(voice,AX_VOICE_RUN);sync(0);
        const auto address=pb_bus+voice->index*320;
        std::array<unsigned char,320> before{};DSPBackendReadMemory(memory,address,before.data(),before.size());
        if(index==0) {
            NativeAXSuppliedCoefficientROM absent;
            Throws([&]{PrepareNativeAXADPCMVoiceFrame(memory,address,absent);},"source4tap ratio1 bypassed missing authentic bank");
            std::array<unsigned char,320> untouched{};DSPBackendReadMemory(memory,address,untouched.data(),untouched.size());
            Check(untouched==before,"missing coefficient failure changed source PB");
        }
        const auto prepared=PrepareNativeAXADPCMVoiceFrame(memory,address,coefficients);
        const auto* expected=c+16+768;
        for(unsigned i=0;i<96;++i)
            Check(prepared.resampled[i]==Signed(LE16(expected+i*2)),"native4tap differs from actual selected owned instructions");
        const auto* end=expected+192;
        Check(BE16(prepared.parameters_after.data()+0xaa)==LE16(end),"nativeSRC fraction differs from actual firmware store");
        for(unsigned i=0;i<4;++i)
            Check(BE16(prepared.parameters_after.data()+0xac+i*2)==LE16(end+2+i*2),"nativeSRC history differs from actual firmware ordered stores");
        Check(prepared.decoded_samples==LE16(end+10),"native accelerator count differs from actual original SRC loop");
        const auto* expected_mix=mix_oracle.data()+12+index*(194+12*388);
        for(unsigned i=0;i<96;++i)
            Check(prepared.enveloped[i]==Signed(LE16(expected_mix+i*2)),"native envelope differs from actual owned034D routine");
        Check(BE16(prepared.parameters_after.data()+0x6a)==LE16(expected_mix+192),
              "native final VE differs from actual firmware store");
        const std::array<unsigned,12> depop_index{{0,4,1,5,2,6,3,7,8,9,10,11}};
        for(unsigned bus=0;bus<12;++bus) {
            const auto* channel=expected_mix+194+bus*388;
            for(unsigned i=0;i<96;++i) {
                const auto bits=LE32(channel+i*4);s32 contribution;std::memcpy(&contribution,&bits,4);
                Check(prepared.buses[bus][i]==contribution,"native signed mix contribution differs from actual owned0C21 routine");
            }
            Check(BE16(prepared.parameters_after.data()+0x14+bus*4)==LE16(channel+384) &&
                  BE16(prepared.parameters_after.data()+0x52+depop_index[bus]*2)==LE16(channel+386),
                  "native final channel volume/depop differs from original DSP stores");
        }
        Check(prepared.parameters_before==before && before!=prepared.parameters_after,
              "prepared source transaction has no original preimage");
        std::array<unsigned char,320> staged{};DSPBackendReadMemory(memory,address,staged.data(),staged.size());
        Check(staged==before,"preparation wrote source PB before whole-frame admission");
        ValidateNativeAXVoiceCommit(memory,prepared);CommitNativeAXVoiceFrame(memory,prepared);
        Check(voice->pb.state==AX_VOICE_RUN,"native hardware directly changed source CPU voice state");
        service_vpb(voice);
        Check(voice->pb.state==BE16(prepared.parameters_after.data()+0x10) &&
              voice->pb.ve.currentVolume==u16(envelope.currentVolume+96*envelope.currentDelta) &&
              ((u32(voice->pb.addr.currentAddressHi)<<16)|voice->pb.addr.currentAddressLo)==
                  BE32(prepared.parameters_after.data()+0x7a),
              "actual source synchronization failed device state/current address/volume receipt");
        Check(prepared.was_running,"active voice contribution omitted a true source voice");
        // Stale source setters are not overwritten by an old prepared job.
        if(index==0) {
            auto changed=voice->pb.src;changed.currentAddressFrac=0x1234;set_src(voice,&changed);service_vpb(voice);
            Throws([&]{CommitNativeAXVoiceFrame(memory,prepared);},"device commit overwrote newer source PB fields");
        }
        set_state(voice,AX_VOICE_STOP);sync(0);release(voice);++frames;
    }
    Check(at==oracle.size(),"selected oracle suffix was ignored");
    NativeDSPMemoryPin owned_pin{};
    if(argc==10) {
        const auto resource=File(argv[6]), encoded=File(argv[7]), owned_oracle=File(argv[9]);
        Check(resource.size()==1248 && encoded.size()==303445 && owned_oracle.size()==12+5*988 &&
              std::memcmp(owned_oracle.data(),"AXOWN337",8)==0,"owned Splash source leaves/oracle differ");
        auto* raw=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(encoded.size(),32));
        std::memcpy(raw,encoded.data(),encoded.size());
        owned_pin=PinNativeDSPMemory(raw,encoded.size(),false,NativeDSPMemoryEncoding::RawBytes);
        const auto bytes=offsetof(SPSoundTable,sound)+5*sizeof(SPSoundEntry)+5*sizeof(SPADPCM);
        auto storage=std::make_unique<std::max_align_t[]>((bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
        auto* owned_table=reinterpret_cast<SPSoundTable*>(storage.get());owned_table->entries=5;
        auto* headers=reinterpret_cast<SPADPCM*>(owned_table->sound+5);
        for(unsigned i=0;i<5;++i) {
            const auto* r=resource.data()+700+i*28;auto& sound=owned_table->sound[i];
            sound.type=BE32(r);sound.sampleRate=BE32(r+4);sound.loopAddr=BE32(r+8);sound.loopEndAddr=BE32(r+12);
            sound.endAddr=BE32(r+16);sound.currentAddr=BE32(r+20);sound.adpcm=nullptr;
            for(unsigned j=0;j<23;++j) {
                const auto value=BE16(resource.data()+840+i*46+j*2);
                std::memcpy(reinterpret_cast<unsigned char*>(headers+i)+j*2,&value,2);
            }
        }
        sp_init(owned_table,OSCachedToPhysical(raw)+0x80000000u,0);
        for(unsigned i=0;i<5;++i) {
            auto* voice=acquire(15,nullptr,0);auto* sound=sp_get(owned_table,i);
            Check(voice && sound && sound->adpcm==headers+i,"owned original SP receiver identity differs");
            sp_prepare(sound,voice,sound->sampleRate);mix_channel(voice,0,0,-960,-960,-960,64,127,0);
            // Fixture-only bank selector: earlier synthetic cases reused this
            // voice. No original cue/event is played or rewritten here.
            set_src_type(voice,AX_SRC_TYPE_4TAP_8K);set_state(voice,AX_VOICE_RUN);sync(0);
            const auto* c=owned_oracle.data()+12+i*988;const auto* expected=c+16+768;
            Check(voice->pb.srcSelect==0 && voice->pb.src.ratioHi==LE32(c)>>16 &&
                  voice->pb.src.ratioLo==u16(LE32(c)),"unchanged ownedSP selected SRC/ratio differs");
            const auto prepared=PrepareNativeAXADPCMVoiceFrame(memory,pb_bus+voice->index*320,coefficients);
            for(unsigned n=0;n<96;++n)
                Check(prepared.resampled[n]==Signed(LE16(expected+n*2)),"owned raw decode/fourtap differs from rawgolden→actual source instruction oracle");
            const auto* end=expected+192;
            Check(prepared.decoded_samples==LE16(end+10) && BE16(prepared.parameters_after.data()+0xaa)==LE16(end),
                  "owned source rate fraction/accelerator count differs");
            for(unsigned n=0;n<4;++n)
                Check(BE16(prepared.parameters_after.data()+0xac+n*2)==LE16(end+2+n*2),"owned source sample history ordering differs");
            CommitNativeAXVoiceFrame(memory,prepared);service_vpb(voice);
            set_state(voice,AX_VOICE_STOP);sync(0);release(voice);++frames;
        }
    }
    // Ordinary stopped source PBs require neither a bank nor any active codec.
    auto* stopped=acquire(15,nullptr,0);sp_prepare(sp_get(table,0),stopped,32000);sync(0);
    const auto stopped_address=pb_bus+stopped->index*320;
    NativeAXSuppliedCoefficientROM absent;
    const auto no_work=PrepareNativeAXADPCMVoiceFrame(memory,stopped_address,absent);
    Check(!no_work.was_running && no_work.parameters_before==no_work.parameters_after,"stopped PB got synthetic active processing");
    release(stopped);
    // Unqualified true source requests remain explicit failures before stores.
    auto* held=acquire(15,nullptr,0);sp_prepare(sp_get(table,0),held,32000);set_state(held,AX_VOICE_RUN);sync(0);
    const auto held_address=pb_bus+held->index*320;
    AXPBLPF lpf{};lpf.on=1;set_lpf(held,&lpf);service_vpb(held);
    Throws([&]{PrepareNativeAXADPCMVoiceFrame(memory,held_address,coefficients);},"sourceLPF silently bypassed");
    lpf.on=0;set_lpf(held,&lpf);set_remote(held,TRUE);service_vpb(held);
    Throws([&]{PrepareNativeAXADPCMVoiceFrame(memory,held_address,coefficients);},"source remote bus silently bypassed");
    set_remote(held,FALSE);set_src_type(held,AX_SRC_TYPE_LINEAR);service_vpb(held);
    Throws([&]{PrepareNativeAXADPCMVoiceFrame(memory,held_address,coefficients);},"source linear selection was forciblyfour-tap");
    set_state(held,AX_VOICE_STOP);sync(0);release(held);
    Check(device.FrameStatus().processed_frames==0,"PB-stage fixture claimed command/output completion");
    ChargedDSPControlWrite(ChargedDSPControlRead()|0x0004);device.Close();
    if(owned_pin.generation)ReleaseNativeDSPMemory(owned_pin);
    ReleaseNativeDSPMemory(sample_pin);ReleaseNativeDSPMemory(drom_pin);
    for(auto it=spans.rbegin();it!=spans.rend();++it) {
        ReleaseNativeDSPMemory(it->pin);OSNativeReleaseStaticMemory(it->mapping);
    }
    DetachNativeDSPControl();DetachNativeDSPMailboxes();DetachNativeDSPMEM1();
    Check(lease.images.empty(),"source backing leases survived real halt/drain/release");
    SDL_UnloadObject(image);ShutdownNativeInterruptController();aurora_shutdown();
    std::cout<<"Native AX active voice stage: "<<checks<<" checks; "<<frames
             <<" actual sourcePB frames, synthetic DROM; authentic SRC/wholekernel/cue remain held\n";
}
}
int main(int argc,char** argv) {
    try{Run(argc,argv);return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
