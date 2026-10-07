#include "platform/ax_bootstrap_device.h"
#include "platform/ax_active_voice.h"
#include "platform/ax_frame_commands.h"
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
#include <revolution/axfx.h>
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
#include <thread>

namespace {
using namespace mscharged::platform;
unsigned checks{};
unsigned source_frame_callbacks{}, source_aux_callbacks{}, effect_allocations{};
void* EffectAllocate(std::size_t n) {auto* p=std::malloc(n);if(p)++effect_allocations;return p;}
void EffectFree(void* p) {if(p){--effect_allocations;std::free(p);}}
void ObserveFrame() { ++source_frame_callbacks; }
void ProduceAux(void* samples, void*) {
    // Synthetic negative contributor through the real source callback slot.
    static_cast<s32*>(samples)[0] = 1;
    ++source_aux_callbacks;
}
void Check(bool value, const char* text) {
    ++checks;
    if (!value) {std::cerr<<"CHECK FAILED: "<<text<<std::endl;throw std::runtime_error(text);}
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
    Check(argc==4, "need source image/identity and owned command arithmetic oracle");
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
    struct DeviceGuard {NativeAXBootstrapDevice& d;~DeviceGuard(){if(std::uncaught_exceptions()){try{AIStopDMA();ServiceNativeAI();ChargedDSPControlWrite(ChargedDSPControlRead()|4);d.Close();}catch(...){}}}} device_guard{device};
    // Real AI precedes original AXInit exactly as the source backend requests.
    // No game backend/factory or source flags are replaced by this leaf gate.
    Throws([&]{device.InitializedGainWords();},"cold device supplied uninitialized gain words");
    AIInit(nullptr);
    source_ax_init();

    Check(is_ax_init() && is_dsp_init(),"actual whole AXInit failed");
    AIStopDMA(); ServiceNativeAI();
    const auto initialized=device.InitializedGainWords();
    Check(initialized==std::array<u16,4>{{0x8000,0x8000,0x8000,0x8000}},"actual init-prefix gain stores differ");
    bool foreign_rejected{};
    std::thread foreign([&]{try{device.InitializedGainWords();}catch(const std::logic_error&){foreign_rejected=true;}});foreign.join();
    Check(foreign_rejected,"foreign thread observed device context outside its actual owner");
    const auto oracle=File(argv[3]);
    Check(oracle.size()==12+8*(196+3456+384) && std::memcmp(oracle.data(),"AXCMD345",8)==0 && LE32(oracle.data()+8)==8,
          "owned selected command oracle differs");
    std::size_t at=12;
    for(unsigned c=0;c<8;++c) {
        const auto* p=oracle.data()+at;at+=196+3456+384;
        const auto gain=NativeAXCommandGainRamp(LE16(p),LE16(p+2));
        for(unsigned i=0;i<96;++i)Check(gain[i]==LE16(p+4+i*2),"96-step ramp differs from actual owned instruction pipeline");
        std::array<NativeAXChannel96,3> mixed{};
        for(unsigned channel=0;channel<3;++channel) {
            NativeAXChannel96 main{},returned{};
            for(unsigned i=0;i<96;++i) {
                const auto m=LE32(p+196+(channel*96+i)*4),r=LE32(p+196+1152+(channel*96+i)*4);
                std::memcpy(&main[i],&m,4);std::memcpy(&returned[i],&r,4);
            }
            mixed[channel]=NativeAXMixAuxReturn(main,returned,gain);
            for(unsigned i=0;i<96;++i) {
                const auto bits=LE32(p+196+2304+(channel*96+i)*4);s32 expected;std::memcpy(&expected,&bits,4);
                Check(mixed[channel][i]==expected,"AUX return arithmetic differs from actual owned40-bit stores");
            }
        }
        const auto pcm=NativeAXPackStereo(mixed[0],mixed[1],gain);
        for(unsigned i=0;i<192;++i)Check(pcm[i]==Signed(LE16(p+196+3456+i*2)),"right/left PCM packing differs from owned instructions");
    }
    auto hooks=Load<decltype(&AXFXSetHooks)>(image,"AXFXSetHooks");
    auto delay_init=Load<decltype(&AXFXDelayInit)>(image,"AXFXDelayInit");
    auto delay_shutdown=Load<decltype(&AXFXDelayShutdown)>(image,"AXFXDelayShutdown");
    auto delay_callback=Load<decltype(&AXFXDelayCallback)>(image,"AXFXDelayCallback");
    auto register_a=Load<decltype(&AXRegisterAuxACallback)>(image,"AXRegisterAuxACallback");
    auto register_b=Load<decltype(&AXRegisterAuxBCallback)>(image,"AXRegisterAuxBCallback");
    auto register_c=Load<decltype(&AXRegisterAuxCCallback)>(image,"AXRegisterAuxCCallback");
    auto process_aux=Load<decltype(&__AXProcessAux)>(image,"__AXProcessAux");
    auto next_frame=Load<decltype(&__AXNextFrame)>(image,"__AXNextFrame");
    auto get_list=Load<decltype(&__AXGetCommandListAddress)>(image,"__AXGetCommandListAddress");
    auto set_master=Load<decltype(&AXSetMasterVolume)>(image,"AXSetMasterVolume");
    auto set_a=Load<decltype(&AXSetAuxAReturnVolume)>(image,"AXSetAuxAReturnVolume");
    auto set_b=Load<decltype(&AXSetAuxBReturnVolume)>(image,"AXSetAuxBReturnVolume");
    auto set_c=Load<decltype(&AXSetAuxCReturnVolume)>(image,"AXSetAuxCReturnVolume");
    auto set_mode=Load<decltype(&AXSetMode)>(image,"AXSetMode");
    auto get_a=Load<decltype(&__AXGetAuxAOutput)>(image,"__AXGetAuxAOutput");
    auto get_b=Load<decltype(&__AXGetAuxBOutput)>(image,"__AXGetAuxBOutput");
    auto get_c=Load<decltype(&__AXGetAuxCOutput)>(image,"__AXGetAuxCOutput");
    auto in_a=Load<decltype(&__AXGetAuxAInput)>(image,"__AXGetAuxAInput");
    auto in_b=Load<decltype(&__AXGetAuxBInput)>(image,"__AXGetAuxBInput");
    auto in_c=Load<decltype(&__AXGetAuxCInput)>(image,"__AXGetAuxCInput");
    auto sync=Load<decltype(&__AXSyncPBs)>(image,"__AXSyncPBs");
    hooks(EffectAllocate,EffectFree);
    std::array<AXFX_DELAY,3> effects{};
    for(unsigned e=0;e<3;++e) {
        auto& fx=effects[e];
        for(unsigned ch=0;ch<3;++ch){fx.delay[ch]=6;fx.feedback[ch]=50;fx.output[ch]=50;}
        Check(delay_init(&fx),"whole original AXFXDelayInit failed");
        Check(fx.active==2 && fx.length[0]==192 && fx.feedbackGain[0]==64 && fx.outGain[0]==64,
              "original AXFXDelay parameters/activation differ");
        // Explicit controlled hardware conformance delay history; no game cue,
        // sample choice, manager/backend or event initialization is substituted.
        for(unsigned ch=0;ch<2;++ch)for(unsigned i=0;i<192;++i)
            fx.line[ch][i]=s32((i*17+ch*13+e*23)%801)-400;
    }
    Check(effect_allocations==9,"original AXFX hooks did not own nine delay lines");
    register_a(delay_callback,&effects[0]);register_b(delay_callback,&effects[1]);register_c(delay_callback,&effects[2]);
    std::array<void*,4> remote{{outputs[2].address,static_cast<unsigned char*>(outputs[2].address)+36,
        static_cast<unsigned char*>(outputs[2].address)+72,static_cast<unsigned char*>(outputs[2].address)+108}};
    NativeAXCommandHistory history{initialized[0],initialized[1],initialized[2],initialized[3],0,true};
    // Counter zero here is an explicit controlled leaf context, not a getter
    // assertion about unspecified0CE4, a full kernel or an attached game device.
    NativeAXSuppliedCoefficientROM absent;
    const auto initial_list=OSCachedToPhysical(get_list());
    const auto initial_zero=ExecuteNativeAXZeroInputFrame(memory,initial_list,128);
    Check(initial_zero.stopped_voices==96 && initial_zero.written_bytes==912,"actual AXOutInit pending first list differs");
    unsigned source_commands=0,nonzero_frames=0;
    std::array<decltype(get_a),3> getters{{get_a,get_b,get_c}}, inputs{{in_a,in_b,in_c}};
    for(unsigned frame=0;frame<8;++frame) {
        sync(0);
        const u16 requested_master=frame%2?0x4000:0x8000;
        const std::array<u16,3> requested_aux{{u16(frame%2?0x1000:0x8000),u16(frame%3?0x3000:0x8000),u16(frame%2?0x6000:0x8000)}};
        set_master(requested_master);set_a(requested_aux[0]);set_b(requested_aux[1]);set_c(requested_aux[2]);
        next_frame(outputs[1].address,outputs[0].address,remote.data());
        const auto list_address=OSCachedToPhysical(get_list());
        std::array<NativeAXChannel96,3> main{};
        const std::array<u16,3> prior{{history.aux_a,history.aux_b,history.aux_c}};
        std::array<void*,3> returned{},uploaded{};
        for(unsigned aux_index=0;aux_index<3;++aux_index) {
            getters[aux_index](&returned[aux_index]);inputs[aux_index](&uploaded[aux_index]);
            Check(returned[aux_index] && uploaded[aux_index] && returned[aux_index]!=uploaded[aux_index],
                  "actual original AUX read/write rings alias");
            const auto gain=NativeAXCommandGainRamp(prior[aux_index],requested_aux[aux_index]);
            auto* source_return=static_cast<s32*>(returned[aux_index]);
            for(unsigned ch=0;ch<3;++ch) {
                NativeAXChannel96 values{};std::copy(source_return+ch*96,source_return+(ch+1)*96,values.begin());
                main[ch]=NativeAXMixAuxReturn(main[ch],values,gain);
            }
        }
        const auto expected=NativeAXPackStereo(main[0],main[1],NativeAXCommandGainRamp(history.master,requested_master));
        const auto output_before=std::vector<unsigned char>(static_cast<unsigned char*>(outputs[0].address),static_cast<unsigned char*>(outputs[0].address)+384);
        auto unqualified=history;unqualified.compressor_counter_known=false;
        Throws([&]{PrepareNativeAXCommandFrame(memory,list_address,128,unqualified,absent);},"unknown compressor history became successful output");
        Check(std::equal(output_before.begin(),output_before.end(),static_cast<unsigned char*>(outputs[0].address)),"unsupported history changed PCM output");
        const auto staged=PrepareNativeAXCommandFrame(memory,list_address,128,history,absent);
        Check(staged.voice_count==96 && staged.active_voices==0 && staged.aux_commands==3 && staged.written_bytes==4368,
              "actual source command/typed AUX/output extent differs");
        Check(std::equal(output_before.begin(),output_before.end(),static_cast<unsigned char*>(outputs[0].address)),"command preparation wrote output early");
        if(frame==0) {
            const auto pb_address=staged.voices.front().parameter_address;
            std::array<unsigned char,4> saved_next{};DSPBackendReadMemory(memory,pb_address,saved_next.data(),saved_next.size());
            const u32 overlapping=pb_address+2;
            const std::array<unsigned char,4> bad_next{{u8(overlapping>>24),u8(overlapping>>16),u8(overlapping>>8),u8(overlapping)}};
            DSPBackendWriteMemory(memory,pb_address,bad_next.data(),bad_next.size());
            bool overlap_rejected{};
            try{PrepareNativeAXCommandFrame(memory,list_address,128,history,absent);}
            catch(const NativeAXCommandError& e){overlap_rejected=e.failure()==NativeAXFailure::InvalidVoiceChain;}
            Check(overlap_rejected,"typed interior pointer became an overlapping PB record");
            Check(std::equal(output_before.begin(),output_before.end(),static_cast<unsigned char*>(outputs[0].address)),"overlapping PB failure changed PCM output");
            DSPBackendWriteMemory(memory,pb_address,saved_next.data(),saved_next.size());
            auto malformed=staged;malformed.voices[0].parameter_address=list_address;
            Throws([&]{CommitNativeAXCommandFrame(memory,malformed,history);},"staged PB/command alias became partial committed output");
            malformed=staged;malformed.voices[1].parameter_address=pb_address+2;
            Throws([&]{CommitNativeAXCommandFrame(memory,malformed,history);},"staged distinct partial PB overlap became committed output");
        }
        if(frame==2) {
            const auto saved=static_cast<s32*>(returned[0])[0];static_cast<s32*>(returned[0])[0]=1000000;
            Throws([&]{PrepareNativeAXCommandFrame(memory,list_address,128,history,absent);},"unsupported compressor attack became silent output");
            Throws([&]{CommitNativeAXCommandFrame(memory,staged,history);},"old AUX snapshot overwrote newer source effect output");
            Check(std::equal(output_before.begin(),output_before.end(),static_cast<unsigned char*>(outputs[0].address)),"rejected AUX snapshot changed PCM");
            static_cast<s32*>(returned[0])[0]=saved;
            auto other=history;other.master^=1;
            Throws([&]{CommitNativeAXCommandFrame(memory,staged,other);},"stale native gain context overwrote current history");
        }
        CommitNativeAXCommandFrame(memory,staged,history);++source_commands;
        auto* pcm=static_cast<s16*>(outputs[0].address);
        for(unsigned i=0;i<192;++i)Check(pcm[i]==expected[i],"actual typed native PCM backing/packed channels differ");
        if(std::any_of(expected.begin(),expected.end(),[](s16 x){return x!=0;}))++nonzero_frames;
        for(unsigned aux_index=0;aux_index<3;++aux_index) {
            auto* source_upload=static_cast<s32*>(uploaded[aux_index]);
            for(unsigned i=0;i<288;++i)Check(source_upload[i]==0,"stopped source PB generated invented AUX send samples");
        }
        Check(history.master==requested_master && history.aux_a==requested_aux[0] && history.aux_b==requested_aux[1] && history.aux_c==requested_aux[2],
              "actual requested native gain history was not retained");
        process_aux();
        for(auto& fx:effects)Check(fx.active==0 && fx.curPos[0]==(frame%2?96u:0u),"original callback activation/96-sample delay cursor differs");
    }
    Check(nonzero_frames>=4,"true original AUX effect callbacks never reached PCM output");
    set_mode(AX_OUTPUT_DPL2);next_frame(outputs[1].address,outputs[0].address,remote.data());
    const auto unsupported_list=OSCachedToPhysical(get_list());
    Throws([&]{PrepareNativeAXCommandFrame(memory,unsupported_list,128,history,absent);},"DPL2 unsupported work became a completed frame");
    set_mode(AX_OUTPUT_STEREO);
    register_a(nullptr,nullptr);register_b(nullptr,nullptr);register_c(nullptr,nullptr);
    for(unsigned i=0;i<3;++i)process_aux();
    for(auto& fx:effects)delay_shutdown(&fx);
    Check(effect_allocations==0,"original AXFXShutdown leaked its hook-owned lines");
    auto acquire=Load<decltype(&AXAcquireVoice)>(image,"AXAcquireVoice");
    auto release=Load<decltype(&AXFreeVoice)>(image,"AXFreeVoice");
    auto set_voice_state=Load<decltype(&AXSetVoiceState)>(image,"AXSetVoiceState");
    auto* active=acquire(15,nullptr,0);Check(active!=nullptr,"actual original active voice allocation failed");
    set_voice_state(active,AX_VOICE_RUN);sync(0);
    next_frame(outputs[1].address,outputs[0].address,remote.data());
    const auto active_list=OSCachedToPhysical(get_list());
    bool coefficient_hold{};
    try{PrepareNativeAXCommandFrame(memory,active_list,128,history,absent);}
    catch(const NativeAXVoiceError& e){coefficient_hold=e.reason()==NativeAXVoiceFailure::MissingCoefficientBank;}
    Check(coefficient_hold,"actual source four-tap active PB bypassed its missing authentic bank");
    set_voice_state(active,AX_VOICE_STOP);sync(0);release(active);
    Check(device.FrameStatus().processed_frames==0,"AUX command conformance became a kernel-ready mailbox endpoint");
    Check(source_commands==8,"genuine source frame producer count differs");
    ChargedDSPControlWrite(GetNativeDSPControlStatus().csr|4|1);
    Throws([&]{device.InitializedGainWords();},"reset device supplied retired initialized gain words");
    Check(is_ax_init(),"native getter/reset rewrote source AX initialization flag");
    device.Close();
    for(auto it=spans.rbegin();it!=spans.rend();++it){ReleaseNativeDSPMemory(it->pin);OSNativeReleaseStaticMemory(it->mapping);}
    Check(lease.images.empty(),"real retained module spans leaked their image lease");
    SDL_UnloadObject(image);
    DetachNativeDSPControl();DetachNativeDSPMailboxes();DetachNativeDSPMEM1();
    ShutdownNativeInterruptController();aurora_shutdown();
    std::cout<<"Native original AX command/AUX gate PASS: "<<checks<<" checks, "<<source_commands
             <<" source lists, "<<nonzero_frames<<" nonzero effect-output frames; authentic FIR/kernel/game cue held\n";
}
} // namespace
int main(int argc,char** argv) {
    try {Run(argc,argv);return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
