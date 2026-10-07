#include "platform/ax_bootstrap_device.h"
#include "platform/ax_storage_abi.h"
#include "platform/dsp_control_abi.h"
#include "platform/interrupt_controller.h"
#include "platform/interrupts.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
extern "C" {
#include <revolution/ax.h>
#include <revolution/dsp.h>
}
#include <array>
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
    const bool preinitialized = argc == 4 && std::strcmp(argv[3], "--preinitialized") == 0;
    Check((argc == 3 || preinitialized) && std::strlen(argv[2]) == 64,
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
    auto source_init = Load<decltype(&DSPInit)>(image, "DSPInit");
    auto is_dsp_init = Load<decltype(&DSPCheckInit)>(image, "DSPCheckInit");
    auto is_ax_init = Load<decltype(&AXIsInit)>(image, "AXIsInit");
    auto init_dsp = Load<decltype(&__AXOutInitDSP)>(image, "__AXOutInitDSP");
    auto source_handler = Load<__OSInterruptHandler>(image, "__DSPHandler");
    Check(!is_dsp_init() && !is_ax_init() && !*actual_current,
          "cold source init flags or task owner differs");
    NativeAXBootstrapDevice device(memory, mail, control, spans.back().mapping.physical_address);
    if (preinitialized) {
        source_init();
        Check(is_dsp_init() && !*actual_current,
              "actual DSPInit branch did not own source initialization");
    }
    // The unmodified whole source method owns its real queue, private task,
    // four callbacks and initialization flag. It returns only after its
    // literal flag wait observes the original instruction-driven handler.
    init_dsp();
    const auto status = device.Status();
    Check(status.phase == NativeAXBootstrapPhase::InitPrefixCompleted &&
              status.loader_words == 10 && status.firmware_instructions == 21,
          "actual original AXOutInitDSP loader/init did not complete");
    Check(is_dsp_init() && !is_ax_init(),
          "DSP source initialization did not preserve full AXInit hold");
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
    auto send = Load<decltype(&DSPSendMailToDSP)>(image, "DSPSendMailToDSP");
    Throws([&] { send(reinterpret_cast<DSPMail>(std::uintptr_t{0xbabe0080})); },
           "unimplemented frame command created successful readiness");
    Check(device.Status().phase == NativeAXBootstrapPhase::Faulted &&
              !is_ax_init() && task->state == DSP_TASK_STATE_1,
          "unsupported frame changed original initialization or task state");
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
    std::cout << "native_ax_init: " << checks << " checks ("
              << (preinitialized ? "preinitialized" : "cold")
              << "); actual whole AXOutInitDSP/private task/queue/init callback; "
                 "full AXInit/frames/SRC held\n";
}
} // namespace
int main(int argc, char** argv) {
    try { Run(argc, argv); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
