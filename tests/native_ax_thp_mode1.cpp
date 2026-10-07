#include "ax_thp_mode1_bridge.h"
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
#include <aurora/dvd.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <dolphin/ai.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_audio.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
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

namespace {
using namespace mscharged::platform;

struct PacketObserver {
    SDL_AudioStreamDataCompleteCallback source;
    void* context;int size;
    unsigned char* Bytes(){return reinterpret_cast<unsigned char*>(this+1);}
};
std::mutex capture_mutex;
std::atomic<bool> capture_enabled{false};
std::uint64_t completed{},nonzero{},peak{};
FILE* capture_file{};
void SDLCALL ObserveComplete(void* context,const void* bytes,int size) {
    auto* observer=static_cast<PacketObserver*>(context);
    if(capture_enabled.load()) {
        std::lock_guard lock(capture_mutex);++completed;bool audible=false;
        for(int i=0;i<observer->size/2;++i) {
            std::int16_t sample;std::memcpy(&sample,observer->Bytes()+i*2,2);
            const auto magnitude=unsigned(sample<0?-int(sample):sample);
            audible|=sample!=0;peak=std::max(peak,std::uint64_t(magnitude));
        }
        nonzero+=audible;
        if(capture_file)std::fwrite(observer->Bytes(),1,observer->size,capture_file);
    }
    observer->source(observer->context,bytes,size);
    std::free(observer);
}

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
    Check(argc==10 && std::strlen(argv[2])==64 && std::strlen(argv[4])==64,
          "need AX/HASH OS/HASH THP DISC MOVIE PCM MILLISECONDS");
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
        std::this_thread::yield();
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
    Check(aurora_dvd_open(argv[6]), "owned source DVD mount failed");
    auto* movie_image=SDL_LoadObject(argv[5]);
    if(!movie_image)throw std::runtime_error(std::string("whole original NL/THP image: ")+SDL_GetError());
    auto open_movie=Load<void(*)(const char*)>(movie_image,"charged_thp_mode1_open");
    auto decode_movie=Load<void(*)()>(movie_image,"charged_thp_mode1_decode");
    auto close_movie=Load<ChargedAXTHPResult(*)()>(movie_image,"charged_thp_mode1_close");
    const BOOL mask=OSDisableInterrupts();
    auto predecessor=AIRegisterDMACallback(nullptr);
    Check(predecessor!=nullptr,"actual AX AI predecessor missing before THP");
    Check(AIRegisterDMACallback(predecessor)==nullptr,"diagnostic predecessor observation changed ownership");
    OSRestoreInterrupts(mask);
    capture_file=std::fopen(argv[8],"wb");Check(capture_file,"private actual PCM capture could not open");
    capture_enabled=true;
    // Owner-delivered AI has a real3ms96-frame deadline. A1ms host sleep
    // plus native Debug processing can miss it; yield does not reset hardware
    // time or manufacture callbacks/PCM. This bounded qualifier keeps serving
    // actual device causes until its real wall-clock deadline.
    const auto before_clock=GetNativeAIClockStatus();BeginNativeAIObservations();
    const auto before_movie=source_frame_callbacks;
    const auto movie_start=SDL_GetTicksNS();
    open_movie(argv[7]);
    const BOOL movie_mask=OSDisableInterrupts();
    auto movie_callback=AIRegisterDMACallback(nullptr);
    Check(movie_callback&&movie_callback!=predecessor,"actualTHPSimple1 did not install its original mixer callback");
    Check(AIRegisterDMACallback(movie_callback)==nullptr,"diagnostic mixer observation changed ownership");
    OSRestoreInterrupts(movie_mask);
    const auto duration=std::stoul(argv[9]);Check(duration>=400&&duration<=4000,"bounded movie duration invalid");
    const auto movie_deadline=movie_start+duration*1000000ull;
    while(SDL_GetTicksNS()<movie_deadline) {
        decode_movie();
        ServiceNativeAI();device.ServiceOwner();
        std::this_thread::yield();
    }
    const auto result=close_movie();
    const auto observed=GetNativeAIObservationStatus();EndNativeAIObservations();
    const auto after_clock=GetNativeAIClockStatus();
    capture_enabled=false;
    const BOOL quit_mask=OSDisableInterrupts();
    auto restored=AIRegisterDMACallback(nullptr);
    Check(restored==predecessor,"actualTHPSimpleQuit did not restore genuine AX predecessor");
    AIRegisterDMACallback(restored);OSRestoreInterrupts(quit_mask);
    Check(result.decoded_frames>0&&result.checks>0,"sourceTHP load/decode/rings did not execute");
    Check(source_frame_callbacks>before_movie+32,"mode1 did not invoke real AX predecessor at hardware cadence");
    Check(device.FrameStatus().completed_frames==source_frame_callbacks&&
              device.FrameStatus().processed_frames==device.FrameStatus().completed_frames,
          "original THP mode1/AX callback lost real command completion");
    const auto after_movie=source_frame_callbacks;
    const auto restored_deadline=SDL_GetTicksNS()+100000000ull;
    while(source_frame_callbacks<after_movie+4&&SDL_GetTicksNS()<restored_deadline) {
        ServiceNativeAI();device.ServiceOwner();std::this_thread::yield();
    }
    Check(source_frame_callbacks>=after_movie+4,"original AX predecessor did not continue after THP quit");
    ShutdownNativeAI();
    std::fclose(capture_file);capture_file=nullptr;
    Check(completed&&nonzero&&peak,"actual consumed combined AX/THP PCM was silent");
    std::cout<<"{\"checks\":"<<checks<<",\"thp_checks\":"<<result.checks
             <<",\"decoded_frames\":"<<result.decoded_frames<<",\"ring_full\":"<<result.ring_full
             <<",\"read_wait\":"<<result.read_wait<<",\"ax_callbacks\":"<<source_frame_callbacks
             <<",\"mode1_ax_callbacks\":"<<(after_movie-before_movie)
             <<",\"completed_pcm\":"<<completed<<",\"nonzero_pcm\":"<<nonzero
             <<",\"peak\":"<<peak<<",\"work_bytes\":"<<result.work_bytes
             <<",\"coalesced_dma\":"<<(after_clock.coalesced_edges-before_clock.coalesced_edges)
             <<",\"dma_edges\":"<<(after_clock.latch_edges-before_clock.latch_edges)
             <<",\"max_service_gap_ns\":"<<after_clock.maximum_service_gap_ns
             <<",\"max_callback_ns\":"<<observed.maximum_callback_duration_ns
             <<",\"total_callback_ns\":"<<observed.total_callback_duration_ns
             <<",\"observed_callbacks\":"<<observed.callbacks_completed<<"}\n";
    aurora_dvd_close();
    register_callback(nullptr);
    ShutdownNativeAI();
    ChargedDSPControlWrite(0x0804);
    normal.Close();device.Close();
    ReleaseNativeDSPMemory(bank_pin);ReleaseNativeDSPMemory(work_pin);
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
    std::cout << "native_ax_thp_mode1: " << checks
              << " checks; whole OSboot→AXInit→originalTHPSimple1/realAXpredecessor/sourcePCM; generated banks, video/MovieInit/GameAudio/CRT held\n";
}
} // namespace
extern "C" bool __real_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream*,const void*,int,SDL_AudioStreamDataCompleteCallback,void*);
extern "C" bool __wrap_SDL_PutAudioStreamDataNoCopy(SDL_AudioStream* stream,const void* bytes,int size,SDL_AudioStreamDataCompleteCallback callback,void* context) {
    auto* observer=static_cast<PacketObserver*>(std::malloc(sizeof(PacketObserver)+size));
    if(!observer)throw std::bad_alloc();
    observer->source=callback;observer->context=context;observer->size=size;
    std::memcpy(observer->Bytes(),bytes,size);
    const bool okay=__real_SDL_PutAudioStreamDataNoCopy(stream,bytes,size,ObserveComplete,observer);
    if(!okay)std::free(observer);return okay;
}
int main(int argc,char** argv) {
    try {Run(argc,argv);std::fflush(nullptr);std::_Exit(0);}
    catch(const std::exception& e) {std::cerr<<"native_ax_thp_mode1: "<<e.what()<<"\n";std::fflush(nullptr);std::_Exit(1);}
}
