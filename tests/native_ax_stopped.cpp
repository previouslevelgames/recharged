#include "platform/ax_bootstrap_device.h"
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
    const std::string negative = argc == 4 ? argv[3] : "--active";
    Check((argc == 3 || argc == 4) &&
              (negative == "--active" || negative == "--studio" || negative == "--aux") &&
              std::strlen(argv[2]) == 64,
          "need actual whole source image and SHA256 identity");
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
    NativeAXBootstrapDevice device(memory, mail, control, spans.back().mapping.physical_address,
                                    NativeAXFrameMode::StoppedVoices);
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
    if (negative == "--active") {
        auto acquire = Load<decltype(&AXAcquireVoice)>(image, "AXAcquireVoice");
        auto set_state = Load<decltype(&AXSetVoiceState)>(image, "AXSetVoiceState");
        auto* voice = acquire(1, nullptr, 0);
        Check(voice != nullptr, "actual source voice pool is unavailable");
        set_state(voice, AX_VOICE_RUN);
    } else if (negative == "--studio") {
        auto depop = Load<decltype(&__AXDepopVoice)>(image, "__AXDepopVoice");
        AXPB contributor{};
        contributor.dpop.aL = 32000;
        depop(&contributor);
    } else {
        auto register_aux = Load<decltype(&AXRegisterAuxACallback)>(image, "AXRegisterAuxACallback");
        register_aux(ProduceAux, nullptr);
        // This source frame consumes the already-prepared non-AUX list; the
        // original next-list producer then includes the real AUX selection.
        source_frame();
        device.ServiceOwner();
        Check(source_aux_callbacks == 1, "original AUX source callback did not execute");
    }
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
    catch (const NativeAXCommandError& error) {
        const auto expected = negative == "--active" ? NativeAXFailure::ActiveVoice :
            negative == "--studio" ? NativeAXFailure::NonzeroStudio : NativeAXFailure::AuxiliaryProcessing;
        correct_failure = error.failure() == expected;
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
    device.Close();
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
    std::cout << "native_ax_stopped: " << checks << " checks (" << negative
              << "); actual whole AXInit, " << frames.completed_frames
              << " real stopped frames/SYNC/YIELD/CONTINUE/AI; active voices/SRC held\n";
}
} // namespace
int main(int argc, char** argv) {
    try { Run(argc, argv); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
