#include "platform/ax_bootstrap_device.h"
#include "platform/ax_command_service.h"
#include "platform/ax_normal_device.h"
#include "platform/os_audio_boot_device.h"
#include "platform/os_boot_environment.h"
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
void Run(int argc, char** argv) {
    Check(argc==5 && std::strlen(argv[2])==64 && std::strlen(argv[4])==64,
          "need wholeAX/sourceOS image identities");
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
    auto* os_image=SDL_LoadObject(argv[3]);Check(os_image!=nullptr,"actual whole OS audio image unavailable");
    auto os_init=Load<void(*)()>(os_image,"__OSInitAudioSystem");
    auto os_stop=Load<void(*)()>(os_image,"__OSStopAudioSystem");
    auto* work=static_cast<unsigned char*>(OSPhysicalToCached(0x01000000));
    const auto work_pin=PinNativeDSPMemory(work,1024,false);
    auto* bank=static_cast<unsigned char*>(OSAllocFromMEM2ArenaLo(12288,32));
    auto put=[](unsigned char* p,unsigned word,u16 value){p[word*2]=value>>8;p[word*2+1]=value;};
    for(unsigned i=0;i<4096;++i)put(bank,i,u16(i*0x351du+0x2abc));
    // Deliberate generated4tap fixture, not authentic hardware bank or a modern
    // resampler. Each phase interpolates the two central samples at small gain.
    for(unsigned b=0;b<4;++b)for(unsigned phase=0;phase<128;++phase) {
        const unsigned at=b*512+phase*4;
        put(bank+8192,at,0);put(bank+8192,at+1,16384-phase*64);
        put(bank+8192,at+2,phase*64);put(bank+8192,at+3,0);
    }
    const auto bank_pin=PinNativeDSPMemory(bank,12288,false);
    const auto bank_physical=OSCachedToPhysical(bank);
    DSPInstructionCore chip(mail,control);chip.LoadInstructionROM(memory,bank_physical);
    chip.LoadCoefficientROM(memory,bank_physical+8192);
    NativeAXSuppliedCoefficientROM coefficients;coefficients.Load(memory,bank_physical+8192);
    ConfigureNativeOSBootEnvironment(NativeOSBootEnvironment::DesktopApplication);
    DSPInstructionRegisters registers{};
    NativeOSAudioBootDevice boot(memory,mail,control,chip,{0x5a5a,0xa5a5,0x6b6b},registers);
    os_init();Check(boot.Status().executed_instructions==16404&&chip.DataWord(0x0ce4)==0,
                   "normal frame history was not established by actual OS boot");
    os_stop();boot.Close();RetireNativeOSBootEnvironment();
    NativeAXBootstrapDevice device(memory, mail, control, spans.back().mapping.physical_address,
                                    NativeAXFrameMode::BootstrapOnly,chip);
    // Real AI precedes original AXInit exactly as the source backend requests.
    // No game backend/factory or source flags are replaced by this leaf gate.
    AIInit(nullptr);
    source_ax_init();
    const auto status = device.Status();
    Check(status.phase == NativeAXBootstrapPhase::InitPrefixCompleted &&
              status.loader_words == 10 && status.firmware_instructions == 21,
          "actual original AXOutInitDSP loader/init did not complete");
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
          "instruction-driven INIT IRQ did not deliver exactly once");
    Check(!ServiceNativeInterruptController(), "original INIT callback repeated");
    NativeAXSuppliedCoefficientROM absent;
    Throws([&]{NativeAXNormalCommandDevice unsupported(device,memory,chip,absent);},
           "normal device guessed a missing coefficient bank");
    DSPInstructionCore other_chip(mail,control);
    Throws([&]{NativeAXNormalCommandDevice other(device,memory,other_chip,coefficients);},
           "normal device borrowed unrelated caller chip state");
    const auto saved_coefficient=bank[8193];bank[8193]^=1;
    NativeAXSuppliedCoefficientROM mismatched;mismatched.Load(memory,bank_physical+8192);bank[8193]=saved_coefficient;
    Throws([&]{NativeAXNormalCommandDevice wrong_bank(device,memory,chip,mismatched);},
           "normal device accepted coefficient words different from its actual retained chip");
    NativeAXNormalCommandDevice normal(device,memory,chip,coefficients);
    Check(normal.Status().history.master==0x8000&&normal.Status().history.compressor_counter_known&&
              normal.Status().history.compressor_counter==0,
          "normal device seeded command history instead of actual initialized cells");
    Throws([&]{NativeAXNormalCommandDevice duplicate(device,memory,chip,coefficients);},
           "normal device allowed two concurrent borrowed processors");

    const auto initial_ai = GetNativeAIStatus();
    Check(initial_ai.initialized && initial_ai.running && initial_ai.dma_bytes == 384 &&
              initial_ai.input_frequency == 32000,
          "original AXOut did not start actual 96-frame 32k DMA");
    Check(register_callback(ObserveFrame) == nullptr,
          "source user callback slot was not initially null");
    const auto deadline = SDL_GetTicksNS() + 1000000000ull;
    while (device.FrameStatus().completed_frames < 8 && SDL_GetTicksNS() < deadline) {
        ServiceNativeAI();
        device.ServiceOwner();
        SDL_Delay(1);
    }
    auto frames = device.FrameStatus();
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
    const auto before_masked = device.FrameStatus();
    const auto before_masked_irqs = GetNativeInterruptControllerStatus().dispatched;
    const BOOL mask = OSDisableInterrupts();
    source_frame();
    device.ServiceOwner();
    Check(device.FrameStatus().processed_frames == before_masked.processed_frames + 1 &&
              device.FrameStatus().completed_frames == before_masked.completed_frames &&
              device.FrameStatus().phase == NativeAXFramePhase::WaitingSyncAcknowledgment &&
              GetNativeInterruptControllerStatus().dispatched == before_masked_irqs,
          "masked original frame invented source acknowledgment or delivered callbacks");
    Throws([&] { OSNativeReleaseStaticMemory(spans[8].mapping); },
           "actual pending output backing lost its pinned image owner");
    OSRestoreInterrupts(mask);
    device.ServiceOwner();
    Check(device.FrameStatus().completed_frames == before_masked.completed_frames + 1 &&
              device.FrameStatus().phase == NativeAXFramePhase::ReadyForListSize &&
              GetNativeInterruptControllerStatus().dispatched == before_masked_irqs + 2 &&
              GetNativeInterruptControllerStatus().dispatch_depth == 0,
          "unmasked SYNC/YIELD or nested source continuation did not complete once");
    auto acquire=Load<decltype(&AXAcquireVoice)>(image,"AXAcquireVoice");
    auto set_state=Load<decltype(&AXSetVoiceState)>(image,"AXSetVoiceState");
    auto set_lpf=Load<decltype(&AXSetVoiceLpf)>(image,"AXSetVoiceLpf");
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
          "normal attachment changed fresh sourceSP/MIX fourtap selection");
    const auto active_address=spans[1].mapping.physical_address+voice->index*320;
    auto set_master=Load<decltype(&AXSetMasterVolume)>(image,"AXSetMasterVolume");
    set_master(0x2000); // Actual SDK command producer, not a copied volume ramp.
    const auto active_before=device.FrameStatus();
    for(unsigned f=0;f<4;++f) {
        source_frame();device.ServiceOwner();
        Check(normal.Status().last_active_voices==1&&device.FrameStatus().stopped_voices==95,
              "real source active PB was omitted by normal protocol");
        Check(device.FrameStatus().completed_frames==active_before.completed_frames+f+1,
              "real active output did not complete sourceSYNC/YIELD/CONTINUE exactly once");
        const auto* pcm=static_cast<const s16*>(spans[8].storage.address);
        Check(std::any_of(pcm,pcm+576,[](s16 value){return value!=0;}),
              "normal protocol fabricated silent output for actual sample input");
    }
    std::array<unsigned char,320> active_pb{};
    DSPBackendReadMemory(memory,active_address,active_pb.data(),active_pb.size());
    auto half=[](const unsigned char* p){return (u16(p[0])<<8)|p[1];};
    auto word=[&](const unsigned char* p){return (u32(half(p))<<16)|half(p+2);};
    auto expected=OSCachedToPhysical(sample)*2+2;
    for(unsigned n=0;n<4*96;++n){if(!(expected&15))expected+=2;++expected;}
    Check(word(active_pb.data()+0x7a)==expected&&half(active_pb.data()+0xaa)==0,
          "normal completion did not retain actual source96frame accelerator/SRC progression");
    Check(normal.Status().history.master==0x2000&&chip.DataWord(0x0ce5)==normal.Status().history.master&&
              chip.DataWord(0x0ce6)==normal.Status().history.aux_a&&
              chip.DataWord(0x0ce4)==normal.Status().history.compressor_counter,
          "native command history diverged from the actual retained chip cells");
    const std::array<u16,5> expected_context{{chip.DataWord(0x0ce4),chip.DataWord(0x0ce5),
        chip.DataWord(0x0ce6),chip.DataWord(0x0ce7),chip.DataWord(0x0ce8)}};
    auto wrong_context=expected_context;wrong_context[2]^=1;
    Throws([&]{chip.CommitNativeDataWords(0x0ce4,wrong_context.data(),expected_context.data(),5);},
           "native context commit overwrote mismatched actual gain history");
    Throws([&]{chip.ValidateNativeDataWords(4095,expected_context.data(),5);},
           "native context admitted an out-of-range data extent");
    Check(chip.DataWord(0x0ce6)==expected_context[2],"failed native context validation changed real data cells");
    Check(source_frame_callbacks==device.FrameStatus().completed_frames,
          "active command attachment replaced source callback ownership");
    std::exception_ptr foreign;std::thread caller([&]{try{normal.Status();}catch(...){foreign=std::current_exception();}});
    caller.join();Check(foreign!=nullptr,"normal device admitted a foreign source owner");
    Throws([&]{normal.Close();},"normal device retired during live source hardware");
    const auto csr=GetNativeDSPControlStatus().csr;
    Throws([&]{ChargedDSPControlWrite(0x0805);},"normal device reset dropped an attached native processor");
    Check(GetNativeDSPControlStatus().csr==csr,"unsupported reset mutated realCSR before failure");
    AXPBLPF filter{};filter.on=1;set_lpf(voice,&filter);
    frames = device.FrameStatus();
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
        correct_failure=error.reason()==NativeAXVoiceFailure::UnsupportedFilter;
    }
    Check(correct_failure, "unsupported contributor did not report its genuine command failure");
    const auto failed = device.FrameStatus();
    Check(device.Status().phase == NativeAXBootstrapPhase::Faulted &&
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
    register_callback(nullptr);
    ShutdownNativeAI();
    ChargedDSPControlWrite(0x0804);
    normal.Close();device.Close();
    ReleaseNativeDSPMemory(sample_pin);ReleaseNativeDSPMemory(bank_pin);ReleaseNativeDSPMemory(work_pin);
    SDL_UnloadObject(os_image);
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
    std::cout << "native_ax_normal: " << checks
              << " checks; whole OSboot→AXInit→actual active PB/output/SYNC/YIELD/CONTINUE; generated bank only, original GameAudio cue remains held\n";
}
} // namespace
int main(int argc, char** argv) {
    try { Run(argc, argv); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
