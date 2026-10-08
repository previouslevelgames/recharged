#include "platform/ax_bootstrap_device.h"
#include "platform/ax_command_service.h"
#include "platform/ax_functional_device.h"
#include "platform/os_audio_boot_abi.h"
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

namespace aurora { extern AuroraConfig g_config; }
void AuroraOSShutdown();

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
    Check(argc==5 && std::strlen(argv[2])==64,
          "need whole originalAX image identity and ready|size|sync|yield phase");
    const std::string phase=argv[4];
    Check(phase=="ready"||phase=="size"||phase=="sync"||phase=="yield",
          "unknown source hardware phase");
    // Pure SDK-memory/clock owner, as in native_alarm_tests: no GX/window.
    // This remains requalifiable against a graphical build's actual SDK.
    aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
    aurora::g_config.mem2Size = 64u * 1024u * 1024u;
    OSInit();
    Check(OSGetArenaLo() && OSGetMEM2ArenaLo(), "sole actual SDK arenas missing");
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
    auto send_mail = Load<decltype(&DSPSendMailToDSP)>(image, "DSPSendMailToDSP");
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
    Throws([]{ChargedOSAudioDSPRead(5);},"missing OS register owner returned a CSR");
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
    auto before_stop=device.Status();
    const auto source_task_state=task->state;
    const auto source_task_flags=task->flags;
    Check(before_stop.frames.phase==NativeAXFramePhase::ReadyForListSize,
          "phase setup needs genuinely acknowledged source protocol");
    Check(ChargedOSAudioDSPRead(27)==0x800c,
          "OS DMA register lost actual running96frame geometry");
    Throws([]{ChargedOSAudioDSPRead(9);},"functional registers invented OS boot ARAM mode");
    Throws([]{ChargedOSAudioDSPWrite(9,0x43);},"functional registers accepted OS boot ARAM mode");
    Throws([]{ChargedOSAudioDSPReadPair(0);},"unsupported mailbox pair read succeeded");
    Throws([]{ChargedOSAudioDSPWritePair(16,0x01000000);},"functional registers accepted OS boot ARAM address");
    Throws([]{ChargedOSAudioIPCRead(0x60);},"functional registers invented OS boot GPIO");
    Throws([]{ChargedOSAudioIPCWrite(0x73,0);},"functional registers accepted OS boot GPIO");
    Throws([]{ChargedOSAudioWorkMemory();},"functional registers supplied OS boot work memory");
    Throws([]{ChargedOSAudioDSPWrite(27,0x000b);},"DMA length change silently became stop");
    Check(GetNativeAIStatus().running && GetNativeAIStatus().dma_bytes==384 &&
              device.Status().protocol.phase==before_stop.protocol.phase &&
              device.Status().frames.processed_frames==before_stop.frames.processed_frames &&
              device.Status().history==before_stop.history,
          "unsupported register request changed device/DMA/history");
    std::exception_ptr foreign;
    std::thread caller([&]{try{ChargedOSAudioDSPRead(5);}catch(...){foreign=std::current_exception();}});
    caller.join();Check(foreign!=nullptr,"OS registers admitted a foreign device owner");
    auto* os_image=SDL_LoadObject(argv[3]);
    Check(os_image!=nullptr,"whole original OS audio image did not load");
    auto source_stop=Load<void(*)()>(os_image,"__OSStopAudioSystem");
    // Retain actual pending source causes at real SDK/MMIO boundaries. No
    // source task/ACK/readiness field is written by the phase test.
    const auto enabled=OSDisableInterrupts();
    Check(enabled,"phase setup did not preserve its actual prior interrupt mask");
    auto unchanged=[&](const NativeAXFunctionalStatus& a,std::uint16_t csr) {
        const auto b=device.Status();
        return ChargedDSPControlRead()==csr && b.native_initialized==a.native_initialized &&
            b.protocol.resets==a.protocol.resets && b.frames.phase==a.frames.phase &&
            b.frames.processed_frames==a.frames.processed_frames &&
            b.frames.completed_frames==a.frames.completed_frames && b.history==a.history;
    };
    {
        const auto a=device.Status();const auto csr=ChargedDSPControlRead();
        Throws([&]{ChargedDSPControlWrite((csr&~0xa0u)|5u);},
               "RESET admitted running actual AI DMA");
        Check(unchanged(a,csr),"rejected live-AI RESET changed CSR/frame/history");
    }
    if(phase=="size")send_mail(reinterpret_cast<DSPMail>(uintptr_t(0x3abe0080u)));
    if(phase=="sync"||phase=="yield")source_frame();
    if(phase=="yield") {
        Check(device.Status().frames.phase==NativeAXFramePhase::WaitingSyncAcknowledgment,
              "actual source frame did not publish SYNC");
        // Exactly one genuine source DSP SYNC handler runs; the actual AI
        // hardware line stays masked while its DMA continues normally.
        __OSMaskInterrupts(OS_INTERRUPTMASK(__OS_INTERRUPT_DSP_AI));
        OSRestoreInterrupts(enabled);
        Check(ServiceNativeInterruptController(),"actual pending SYNC IRQ was not delivered");
        (void)OSDisableInterrupts();
        (void)ChargedOSAudioDSPRead(5);
    }
    const auto expected=phase=="ready"?NativeAXFramePhase::ReadyForListSize:
        phase=="size"?NativeAXFramePhase::ReadyForListAddress:
        phase=="sync"?NativeAXFramePhase::WaitingSyncAcknowledgment:NativeAXFramePhase::WaitingContinue;
    before_stop=device.Status();
    Check(before_stop.frames.phase==expected,"actual source request reached a different phase");
    if(phase=="sync"||phase=="yield") {
        const auto csr=ChargedDSPControlRead();
        Throws([&]{ChargedDSPControlWrite(csr|5u);},"RESET admitted unread source mail/IRQ");
        Check(unchanged(before_stop,csr),"rejected pending-mail RESET changed CSR/frame/history");
    }
    const auto source_callbacks=source_frame_callbacks;
    std::cout << "before original OS stop: phase=" << phase
              << " processed=" << before_stop.frames.processed_frames
              << " completed=" << before_stop.frames.completed_frames << '\n';
    const auto irqs_before_stop=GetNativeInterruptControllerStatus().dispatched;
    const auto tick_before=OSGetTick();
    source_stop();
    OSRestoreInterrupts(enabled);
    const auto after_stop=device.Status();
    Check(static_cast<u32>(OSGetTick()-tick_before)>=44,
          "whole source stop omitted its original44tick wait");
    Check(!GetNativeAIStatus().running && !GetNativeAIStatus().interrupt_pending &&
              !GetNativeAIStatus().callback_active && GetNativeAIStatus().retained_blocks==0 &&
              GetNativeAIStatus().queued_input_bytes==0,
          "whole source stop did not stop/drain actual AI DMA");
    Check(!after_stop.native_initialized && after_stop.protocol.phase==NativeAXBootstrapPhase::Cold &&
              after_stop.protocol.hardware_halted && after_stop.protocol.resets==before_stop.protocol.resets+1 &&
              after_stop.protocol.firmware_instructions==0 && !after_stop.history.compressor_counter_known &&
              !GetNativeDSPMailboxStatus().cpu_mail_full && !GetNativeDSPMailboxStatus().dsp_mail_full &&
              !(ChargedDSPControlRead()&0x81),
          "whole source stop did not perform actual HALT/mail/W1C/reset");
    Check(is_ax_init() && is_dsp_init() && *actual_current==task &&
              task->state==source_task_state && task->flags==source_task_flags &&
              source_frame_callbacks==source_callbacks &&
              GetNativeInterruptControllerStatus().dispatched==irqs_before_stop,
          "native stop altered source flags/task or fabricated source callback/IRQ");
    device.ServiceOwner();ServiceNativeAI();device.ServiceOwner();
    Check(device.Status().protocol.phase==NativeAXBootstrapPhase::Cold &&
              device.Status().frames.phase==NativeAXFramePhase::Unavailable &&
              device.Status().frames.processed_frames==0 &&
              device.Status().frames.completed_frames==0 &&
              device.Status().initialization_count==before_stop.initialization_count &&
              source_frame_callbacks==source_callbacks,
          "later real owner service restarted reset DSP or fabricated a source callback");
    register_callback(nullptr);
    ShutdownNativeAI();
    device.Close();
    Throws([]{ChargedOSAudioDSPRead(5);},"retired register route retained a device pointer");
    Throws([&]{device.Status();},"retired functional device exposed live history");
    DetachNativeDSPControl();
    DetachNativeDSPMailboxes();
    ShutdownNativeInterruptController();
    for (auto iter = spans.rbegin(); iter != spans.rend(); ++iter) {
        ReleaseNativeDSPMemory(iter->pin);
        OSNativeReleaseStaticMemory(iter->mapping);
    }
    Check(lease.images.empty(), "actual static memory lease survived device retirement");
    SDL_UnloadObject(os_image);
    SDL_UnloadObject(image);
    DetachNativeDSPMEM1();
    AuroraOSShutdown();
    std::cout << "native_os_audio_functional_stop: " << checks
              << " checks; whole AXInit/8 source frames → original OS stop/HALT/DMA/mail/reset; phase="
              << phase << "\n";
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
