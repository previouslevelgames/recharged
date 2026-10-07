#include "platform/ax_bootstrap_device.h"
#include "platform/ax_command_service.h"
#include "platform/ax_functional_device.h"
#include "platform/ax_studio_depop.h"
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
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <cstdlib>

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
    if (!value) {std::cerr << "CHECK FAILED: " << text << "\n";throw std::runtime_error(text);}
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
void Run(int argc, char** argv) {
    Check(argc==3 && std::strlen(argv[2])==64,
          "need whole originalAX image identity");
    const auto data = std::filesystem::absolute("sdk-data").string();
    std::filesystem::create_directories(data);
    AuroraConfig config{};
    config.appName = "Original AX output DSP initialization";
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
    NativeAXFunctionalBindings bindings{};
    for(unsigned i=0;i<spans.size();++i)bindings.addresses[i]=spans[i].mapping.physical_address;
    Throws([&]{NativeAXFunctionalDevice wrong_policy(memory,mail,control,bindings,
        NativeAXCoefficientPolicy::SuppliedBank);},"functional mode silently chose a coefficient policy");
    auto overlap=bindings;overlap.addresses[1]=overlap.addresses[0];
    Throws([&]{NativeAXFunctionalDevice wrong_owner(memory,mail,control,overlap,
        NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1);},"functional init admitted overlapping source spans");
    auto* incorrect_image=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(8192,32));
    std::memcpy(incorrect_image,spans[12].storage.address,8192);incorrect_image[1]^=1;
    const auto incorrect_pin=PinNativeDSPMemory(incorrect_image,8192,false);
    auto incorrect_bindings=bindings;incorrect_bindings.addresses[12]=OSCachedToPhysical(incorrect_image);
    Throws([&]{NativeAXFunctionalDevice incorrect_task(memory,mail,control,incorrect_bindings,
        NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1);},"native loader admitted a different source firmware");
    ReleaseNativeDSPMemory(incorrect_pin);
    Check(!is_ax_init()&&!is_dsp_init()&&!GetNativeDSPMailboxStatus().dsp_mail_full,
          "failed native task admission changed actual source flags/mail");
    NativeAXFunctionalDevice device(memory,mail,control,bindings,
        NativeAXCoefficientPolicy::NativeWindowedSinc4TapV1);
    Check(!device.Status().native_initialized && device.Status().protocol.phase==NativeAXBootstrapPhase::Cold &&
          device.Status().protocol.firmware_instructions==0 && !GetNativeDSPMailboxStatus().dsp_mail_full,
          "functional attach fabricated initial completion/mail/ISA work");
    // Real AI precedes original AXInit exactly as the source backend requests.
    // No game backend/factory or source flags are replaced by this leaf gate.
    AIInit(nullptr);
    source_ax_init();
    const auto status = device.Status().protocol;
    Check(status.phase == NativeAXBootstrapPhase::NativeKernelInitialized &&
              status.loader_words == 10 && status.firmware_instructions == 0,
          "actual original AXOutInitDSP native loader/init did not complete");
    Check(is_dsp_init() && is_ax_init(),
          "actual whole AXInit did not own its source initialization");
    Check(*actual_current == task && task->state == DSP_TASK_STATE_1 &&
              task->flags == DSP_TASK_ACTIVE && task->prio == 0,
          "real original private task/handler state differs");
    Check(task->iramMmemAddr == spans.back().storage.address &&
              task->iramMmemLen == 8192 && task->iramDspAddr == 0,
          "original AX task firmware request differs");
    Check(task->dramMmemAddr == spans[11].storage.address &&
              task->dramMmemLen == 64 && task->dramDspAddr == 0x0cd2,
          "original AX task context request differs");
    Check(task->startVector == 0x10 && task->resumeVector == 0x37 &&
              task->initCallback && task->resumeCallback && task->doneCallback &&
              task->requestCallback,
          "actual original vectors/callback ownership differs");
    Check(__OSGetInterruptHandler(__OS_INTERRUPT_DSP_DSP) == source_handler,
          "whole source handler was not installed");
    Check(GetNativeInterruptControllerStatus().dispatched == 1 &&
              !(ChargedDSPControlRead() & 0x80),
          "actual native initialization INIT IRQ did not deliver exactly once");
    Check(!ServiceNativeInterruptController(), "original INIT callback repeated");
    Check(device.Status().native_initialized && device.Status().initialization_count==1 &&
              device.Status().history.master==0x8000 && device.Status().history.compressor_counter_known &&
              device.Status().history.compressor_counter==0,
          "native processor resources did not complete before source callback");
    const auto initial_ai = GetNativeAIStatus();
    Check(initial_ai.initialized && initial_ai.running && initial_ai.dma_bytes == 384 &&
              initial_ai.input_frequency == 32000,
          "original AXOut did not start actual 96-frame 32k DMA");
    Check(register_callback(ObserveFrame) == nullptr,
          "source user callback slot was not initially null");
    const auto deadline = SDL_GetTicksNS() + 1000000000ull;
    while (device.Status().frames.completed_frames < 8 && SDL_GetTicksNS() < deadline) {
        ServiceNativeAI();
        device.ServiceOwner();
        std::this_thread::yield();
    }
    auto frames = device.Status().frames;
    Check(frames.completed_frames >= 8 && frames.processed_frames == frames.completed_frames,
          "real source AI did not process and complete stopped frames");
    Check(frames.sync_interrupts == frames.completed_frames &&
              frames.yield_interrupts == frames.completed_frames &&
              frames.source_continues == frames.completed_frames,
          "source SYNC/ack/YIELD/CONTINUE lifecycle differs");
    Check(source_frame_callbacks == frames.completed_frames &&
              GetNativeInterruptControllerStatus().dispatched == 1 + 2 * frames.completed_frames,
          "original source callbacks/real DSP causes differ from processed requests");
    Check(frames.stopped_voices == 96 && frames.stereo_frames == 96 &&
              frames.remote_samples == 18 && frames.written_bytes == 912,
          "actual command service output geometry differs");
    const auto clock = GetNativeAIClockStatus();
    Check(clock.latch_edges >= frames.completed_frames && clock.transferred_cells > 0 &&
              GetNativeAIStatus().dispatched_callbacks == source_frame_callbacks,
          "actual AI clock/transfer/callback evidence missing");
    for (std::size_t index : {std::size_t(8), std::size_t(9), std::size_t(10)}) {
        const auto* bytes = static_cast<const unsigned char*>(spans[index].storage.address);
        Check(std::all_of(bytes, bytes + spans[index].storage.bytes,
                          [](unsigned char byte) { return byte == 0; }),
              "genuine stopped-voice PCM/surround/remote output is nonzero");
    }
    AIStopDMA();
    Check(!GetNativeAIStatus().running, "real AI did not stop before negative device work");
    const auto before_masked = device.Status().frames;
    const auto before_masked_irqs = GetNativeInterruptControllerStatus().dispatched;
    const BOOL mask = OSDisableInterrupts();
    source_frame();
    device.ServiceOwner();
    Check(device.Status().frames.processed_frames == before_masked.processed_frames + 1 &&
              device.Status().frames.completed_frames == before_masked.completed_frames &&
              device.Status().frames.phase == NativeAXFramePhase::WaitingSyncAcknowledgment &&
              GetNativeInterruptControllerStatus().dispatched == before_masked_irqs,
          "masked original frame invented source acknowledgment or delivered callbacks");
    Throws([&] { OSNativeReleaseStaticMemory(spans[8].mapping); },
           "actual pending output backing lost its pinned image owner");
    OSRestoreInterrupts(mask);
    device.ServiceOwner();
    Check(device.Status().frames.completed_frames == before_masked.completed_frames + 1 &&
              device.Status().frames.phase == NativeAXFramePhase::ReadyForListSize &&
              GetNativeInterruptControllerStatus().dispatched == before_masked_irqs + 2 &&
              GetNativeInterruptControllerStatus().dispatch_depth == 0,
          "unmasked SYNC/YIELD or nested source continuation did not complete once");
    auto acquire=Load<decltype(&AXAcquireVoice)>(image,"AXAcquireVoice");
    auto set_state=Load<decltype(&AXSetVoiceState)>(image,"AXSetVoiceState");
    auto set_lpf=Load<decltype(&AXSetVoiceLpf)>(image,"AXSetVoiceLpf");
    auto get_lpf_coefs=Load<decltype(&AXGetLpfCoefs)>(image,"AXGetLpfCoefs");
    auto set_remote=Load<decltype(&AXSetVoiceRmtOn)>(image,"AXSetVoiceRmtOn");
    auto mix_init=Load<decltype(&MIXInit)>(image,"MIXInit");
    auto mix_channel=Load<decltype(&MIXInitChannel)>(image,"MIXInitChannel");
    auto sp_init=Load<decltype(&SPInitSoundTable)>(image,"SPInitSoundTable");
    auto sp_get=Load<decltype(&SPGetSoundEntry)>(image,"SPGetSoundEntry");
    auto sp_prepare=Load<decltype(&SPPrepareSound)>(image,"SPPrepareSound");
    mix_init();
    auto* sample=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(8192,32));
    std::memset(sample,0x13,8192);for(unsigned i=0;i<8192;i+=8)sample[i]=8;
    const auto sample_pin=PinNativeDSPMemory(sample,8192,false);
    alignas(SPSoundTable) std::array<unsigned char,
        offsetof(SPSoundTable,sound)+sizeof(SPSoundEntry)+sizeof(SPADPCM)> table_storage{};
    auto* table=reinterpret_cast<SPSoundTable*>(table_storage.data());table->entries=1;
    table->sound[0].sampleRate=32000;table->sound[0].type=0;
    table->sound[0].currentAddr=2;table->sound[0].endAddr=16383;
    auto* codec=reinterpret_cast<SPADPCM*>(table->sound+1);codec->adpcm.pred_scale=8;
    sp_init(table,OSCachedToPhysical(sample)+0x80000000u,0);
    auto* voice=acquire(15,nullptr,0);Check(voice!=nullptr,"actual source active voice allocation failed");
    sp_prepare(sp_get(table,0),voice,32000);
    mix_channel(voice,0,0,-960,-960,-960,64,127,0);set_state(voice,AX_VOICE_RUN);
    Check(voice->pb.srcSelect==0&&voice->pb.coefSelect==0&&voice->pb.src.ratioHi==1,
          "native attachment changed fresh sourceSP/MIX fourtap selection");
    const auto active_address=spans[1].mapping.physical_address+voice->index*320;
    auto set_master=Load<decltype(&AXSetMasterVolume)>(image,"AXSetMasterVolume");
    set_master(0x2000); // Actual SDK command producer, not a copied volume ramp.
    AXPBLPF filter{};filter.on=1;get_lpf_coefs(2000,&filter.a0,&filter.b0);set_lpf(voice,&filter);
    const auto active_before=device.Status().frames;
    for(unsigned f=0;f<4;++f) {
        source_frame();device.ServiceOwner();
        Check(device.Status().last_active_voices==1&&device.Status().frames.stopped_voices==95,
              "real source active PB was omitted by native protocol");
        Check(device.Status().frames.completed_frames==active_before.completed_frames+f+1,
              "real active output did not complete sourceSYNC/YIELD/CONTINUE exactly once");
        const auto* pcm=static_cast<const s16*>(spans[8].storage.address);
        Check(std::any_of(pcm,pcm+576,[](s16 value){return value!=0;}),
              "native protocol fabricated silent output for actual sample input");
        std::array<unsigned char,320> filtered_pb{};
        DSPBackendReadMemory(memory,active_address,filtered_pb.data(),filtered_pb.size());
        auto half=[](const unsigned char* p){return (u16(p[0])<<8)|p[1];};
        Check(half(filtered_pb.data()+0xba)==1&&half(filtered_pb.data()+0xbe)==filter.a0&&
                  half(filtered_pb.data()+0xc0)==filter.b0&&half(filtered_pb.data()+0xbc)!=0,
              "actual active command did not retain source LPF request/history");
    }
    std::array<unsigned char,320> active_pb{};
    DSPBackendReadMemory(memory,active_address,active_pb.data(),active_pb.size());
    auto half=[](const unsigned char* p){return (u16(p[0])<<8)|p[1];};
    auto word=[&](const unsigned char* p){return (u32(half(p))<<16)|half(p+2);};
    auto expected=OSCachedToPhysical(sample)*2+2;
    for(unsigned n=0;n<4*96;++n){if(!(expected&15))expected+=2;++expected;}
    Check(word(active_pb.data()+0x7a)==expected&&half(active_pb.data()+0xaa)==0,
          "native completion did not retain actual source96frame accelerator/SRC progression");
    Check(device.Status().history.master==0x2000 && device.Status().history.compressor_counter_known &&
              device.Status().history.compressor_counter==0,
          "real committed output did not update private native history");
    Check(source_frame_callbacks==device.Status().frames.completed_frames,
          "active command attachment replaced source callback ownership");
    std::exception_ptr foreign;std::thread caller([&]{try{device.Status();}catch(...){foreign=std::current_exception();}});
    caller.join();Check(foreign!=nullptr,"functional device admitted a foreign source owner");
    Throws([&]{device.Close();},"functional device retired during live source hardware");
    const BOOL reset_mask=OSDisableInterrupts();
    source_frame();
    const auto waiting=device.Status().frames;
    Throws([&]{ChargedDSPControlWrite(0x0805);},"native reset discarded unacknowledged original source work");
    Check(device.Status().frames.completed_frames==waiting.completed_frames &&
              (GetNativeDSPControlStatus().csr&0x80),"failed pending reset invented a drain");
    OSRestoreInterrupts(reset_mask);device.ServiceOwner();
    Check(device.Status().frames.phase==NativeAXFramePhase::ReadyForListSize,
          "source acknowledgment did not resume after rejected pending reset");
    set_remote(voice,TRUE);
    frames = device.Status().frames;
    const auto irq_count = GetNativeInterruptControllerStatus().dispatched;
    for (std::size_t index : {std::size_t(8), std::size_t(10)}) {
        auto* bytes = static_cast<unsigned char*>(spans[index].storage.address);
        std::fill(bytes, bytes + spans[index].storage.bytes, 0x5a);
    }
    std::array<std::vector<unsigned char>, 3> before_outputs;
    for (std::size_t i = 0; i < before_outputs.size(); ++i) {
        auto& span = spans[8 + i];
        auto* bytes = static_cast<unsigned char*>(span.storage.address);
        before_outputs[i].assign(bytes, bytes + span.storage.bytes);
    }
    bool correct_failure{};
    try { source_frame(); }
    catch (const NativeAXVoiceError& error) {
        correct_failure=error.reason()==NativeAXVoiceFailure::UnsupportedRemote;
    }
    Check(correct_failure, "unsupported contributor did not report its genuine command failure");
    const auto failed = device.Status().frames;
    Check(device.Status().protocol.phase == NativeAXBootstrapPhase::Faulted &&
              failed.processed_frames == frames.processed_frames &&
              failed.completed_frames == frames.completed_frames &&
              failed.sync_interrupts == frames.sync_interrupts &&
              failed.yield_interrupts == frames.yield_interrupts &&
              GetNativeInterruptControllerStatus().dispatched == irq_count,
          "unsupported work published output completion or real IRQ");
    for (std::size_t i = 0; i < before_outputs.size(); ++i)
        Check(std::memcmp(spans[8 + i].storage.address, before_outputs[i].data(),
                          before_outputs[i].size()) == 0,
              "unsupported work altered actual source output backing");
    Check(is_ax_init() && task->state == DSP_TASK_STATE_1 && task->flags == DSP_TASK_ACTIVE,
          "host fault changed original source initialization/task flags");
    // Real platform RESET/HALT discards only private native state. The original
    // source task/globals stay owned by their source; invoke its whole original
    // loader again for this bounded hardware restart (not a GameAudio restart).
    set_remote(voice,FALSE);set_state(voice,AX_VOICE_STOP);
    const auto original_task_state=task->state;const auto original_task_flags=task->flags;
    const auto previous_init_count=device.Status().initialization_count;
    const auto original_hardware_masks=GetNativeDSPControlStatus().csr&0x0150;
    ChargedDSPControlWrite(original_hardware_masks|0x0804);
    ChargedDSPControlWrite(original_hardware_masks|0x0805);
    const auto cold_reset=device.Status();
    Check(!cold_reset.native_initialized&&!cold_reset.history.compressor_counter_known&&
              cold_reset.history.master==0&&cold_reset.protocol.phase==NativeAXBootstrapPhase::Cold&&
              cold_reset.protocol.resets==1&&cold_reset.protocol.firmware_instructions==0,
          "native RESET preserved stale history or claimed ISA work");
    Check(is_ax_init()&&is_dsp_init()&&*actual_current==task&&task->state==original_task_state&&
              task->flags==original_task_flags,"native RESET changed original source flags/task ownership");
    ChargedDSPControlWrite(original_hardware_masks|0x0800);
    Check(device.Status().protocol.phase==NativeAXBootstrapPhase::LoaderReady&&
              !device.Status().native_initialized,"loader availability invented native task initialization");
    auto source_boot_task=Load<void(*)(DSPTask*)>(image,"__DSP_boot_task");
    const auto irq_before_reload=GetNativeInterruptControllerStatus().dispatched;
    source_boot_task(task);device.ServiceOwner();
    Check(device.Status().protocol.phase==NativeAXBootstrapPhase::NativeKernelInitialized&&
              device.Status().initialization_count==previous_init_count+1&&
              device.Status().history.master==0x8000&&device.Status().history.compressor_counter==0&&
              device.Status().protocol.firmware_instructions==0&&
              GetNativeInterruptControllerStatus().dispatched==irq_before_reload+1,
          "actual original reload did not reinitialize native history and deliver one true INIT cause");
    // Source Stop, __AXSyncPBs and __AXPrintStudio own actual PB→Studio
    // accumulation and fade history. The native processor only executes its
    // literal Setup arithmetic, then existing original list output operations.
    DSPBackendReadMemory(memory,active_address,active_pb.data(),active_pb.size());
    constexpr unsigned dpop_index[12]={0,4,8,1,5,9,2,6,10,3,7,11};
    std::array<s32,12> remaining{};
    for(unsigned i=0;i<12;++i) {
        const u16 bits=half(active_pb.data()+0x52+dpop_index[i]*2);
        s16 value;std::memcpy(&value,&bits,2);remaining[i]=value;
    }
    Check(remaining[0]!=0||remaining[1]!=0,"actual active PB has no depop contributor");
    const auto source_callbacks_before_depop=source_frame_callbacks;
    bool exhausted{};unsigned depop_frames{};
    for(;depop_frames<32&&!exhausted;++depop_frames) {
        const auto prior=device.Status();
        const auto prior_irqs=GetNativeInterruptControllerStatus().dispatched;
        source_frame();device.ServiceOwner();
        const auto completed=device.Status();
        const auto requested=ReadNativeAXCommandList(memory,completed.frames.last_list_address,128);
        u32 output_address{};u16 requested_gain{};
        for(unsigned i=0;i<requested.command_count;++i)if(requested.commands[i].opcode==NativeAXOpcode::Output) {
            const auto& c=requested.commands[i];requested_gain=c.arguments[0];
            output_address=(u32(c.arguments[3])<<16)|c.arguments[4];
        }
        Check(output_address!=0,"actual Stop list omitted output request");
        Check(completed.protocol.phase==NativeAXBootstrapPhase::NativeKernelInitialized&&
                  completed.frames.completed_frames==prior.frames.completed_frames+1&&
                  completed.frames.processed_frames==prior.frames.processed_frames+1&&
                  GetNativeInterruptControllerStatus().dispatched==prior_irqs+2&&
                  completed.last_active_voices==0&&completed.frames.stopped_voices==96,
              "actual source Stop did not complete one real Studio/output/SYNC/YIELD/CONTINUE frame");
        std::array<unsigned char,120> studio_wire{};
        DSPBackendReadMemory(memory,spans[7].mapping.physical_address,studio_wire.data(),120);
        bool any{};
        for(unsigned i=0;i<12;++i) {
            s32 expected_value{};s16 expected_delta{};
            if(remaining[i]/96) {
                const auto fade=std::max<s32>(-20,std::min<s32>(20,remaining[i]/96));
                expected_value=remaining[i];expected_delta=-fade;remaining[i]-=fade*96;
            } else remaining[i]=0;
            const auto bits=word(studio_wire.data()+i*6);s32 value;std::memcpy(&value,&bits,4);
            const auto dbits=half(studio_wire.data()+i*6+4);s16 delta;std::memcpy(&delta,&dbits,2);
            Check(value==expected_value&&delta==expected_delta,"original packed Studio accumulation/fade history differs");
            any=any||value!=0;
        }
        const auto lv=word(studio_wire.data()),rv=word(studio_wire.data()+6);
        s32 left,right;std::memcpy(&left,&lv,4);std::memcpy(&right,&rv,4);
        const auto ld=half(studio_wire.data()+4),rd=half(studio_wire.data()+10);
        s16 left_delta,right_delta;std::memcpy(&left_delta,&ld,2);std::memcpy(&right_delta,&rd,2);
        const auto gain=NativeAXCommandGainRamp(prior.history.master,requested_gain);
        const auto expected_pcm=NativeAXPackStereo(NativeAXStudioDepop96(left,left_delta),
            NativeAXStudioDepop96(right,right_delta),gain);
        std::array<unsigned char,384> pcm_wire{};
        DSPBackendReadMemory(memory,output_address,pcm_wire.data(),384);
        for(unsigned i=0;i<192;++i)Check(half(pcm_wire.data()+i*2)==u16(expected_pcm[i]),
            "true source Stop PCM differs from independently qualified Setup/ramp/output arithmetic");
        Check(source_frame_callbacks==source_callbacks_before_depop+depop_frames+1,
              "actual Stop replaced source callback ownership");
        exhausted=!any;
    }
    Check(exhausted&&depop_frames>1,"original depop totals did not decay through their true source frames");
    // Controlled negative through the actual source accumulation API. Remote
    // Setup arithmetic is qualified independently; remote output/mix remains
    // an explicit unsupported request, never fabricated silent completion.
    auto source_depop=Load<decltype(&__AXDepopVoice)>(image,"__AXDepopVoice");
    AXPB remote_contributor{};remote_contributor.rmtDpop.aMain0=200;
    source_depop(&remote_contributor);
    std::array<unsigned char,1152> stopped_output_before{};
    std::memcpy(stopped_output_before.data(),spans[8].storage.address,1152);
    const auto irq_before_depop=GetNativeInterruptControllerStatus().dispatched;
    const auto before_remote=device.Status();bool exact_depop_hold{};
    try {source_frame();device.ServiceOwner();}
    catch(const NativeAXCommandError& error){exact_depop_hold=error.failure()==NativeAXFailure::NonzeroStudio;}
    Check(exact_depop_hold&&device.Status().protocol.phase==NativeAXBootstrapPhase::Faulted&&
              device.Status().frames.processed_frames==before_remote.frames.processed_frames&&
              device.Status().frames.completed_frames==before_remote.frames.completed_frames&&
              device.Status().history==before_remote.history&&
              GetNativeInterruptControllerStatus().dispatched==irq_before_depop&&
              std::memcmp(stopped_output_before.data(),spans[8].storage.address,1152)==0,
          "unsupported remote Studio hold was hidden by partial PCM/history/IRQ success");
    register_callback(nullptr);
    ShutdownNativeAI();
    ChargedDSPControlWrite(0x0804);
    device.Close();
    Throws([&]{device.Status();},"retired functional device exposed live history");
    ReleaseNativeDSPMemory(sample_pin);
    DetachNativeDSPControl();
    DetachNativeDSPMailboxes();
    ShutdownNativeInterruptController();
    for (auto iter = spans.rbegin(); iter != spans.rend(); ++iter) {
        ReleaseNativeDSPMemory(iter->pin);
        OSNativeReleaseStaticMemory(iter->mapping);
    }
    Check(lease.images.empty(), "actual static memory lease survived device retirement");
    SDL_UnloadObject(image);
    DetachNativeDSPMEM1();
    aurora_shutdown();
    std::cout << "native_ax_functional: " << checks
              << " checks; native cold/reset→whole AXInit→activePB/output/SYNC/YIELD/CONTINUE; explicit nativefilter, no ROM/ISA/game cue\n";
}
} // namespace
int main(int argc, char** argv) {
    std::set_terminate([] {
        const auto pending=std::current_exception();
        if(pending)try {std::rethrow_exception(pending);}catch(const std::exception& error){std::cerr << "UNWOUND LIVE DEVICE: " << error.what() << '\n';}
        std::abort();
    });
    try { Run(argc, argv); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
