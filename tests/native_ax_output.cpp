#include "fixtures/native_ax_output_source.h"
#include "platform/dsp_memory.h"
#include "platform/dsp_memory_abi.h"
#include "platform/dsp_mailbox.h"
#include "platform/interrupt_controller.h"
#include "platform/ai.h"
#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <dolphin/ai.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_error.h>
extern "C" {
#include <revolution/ax.h>
#include <revolution/dsp.h>
}
#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

namespace {
using namespace mscharged::platform;
unsigned checks{}, retired{};
void Check(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
template<class F> void Throws(F&& function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message);
}
template<class F> F Load(SDL_SharedObject* image, const char* name) {
    auto* symbol = SDL_LoadFunction(image, name);
    if (!symbol) throw std::runtime_error(std::string("Missing actual source provider: ") + name);
    return reinterpret_cast<F>(symbol);
}
struct Owner {
    std::string path, identity;
    SDL_SharedObject* initial{};
    std::vector<SDL_SharedObject*> leases;
    std::vector<OSNativeStaticMemory> mappings;
    std::vector<NativeDSPMemoryPin> pins;
    std::array<ChargedAXStorage,13> storage{};
    ChargedAXStorage task{};
    ChargedAXOutputMemorySnapshot early{};
    bool reserved{};
    static BOOL Retain(void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        auto* image = SDL_LoadObject(self.path.c_str());
        if (!image) return FALSE;
        try { self.leases.push_back(image); }
        catch (...) { SDL_UnloadObject(image); return FALSE; }
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& self = *static_cast<Owner*>(context);
        auto* image = self.leases.back(); self.leases.pop_back();
        SDL_UnloadObject(image);
    }
};
Owner* loading{};
const char* names[] = {"AXCommandLists", "AXPB", "AXITD", "AXAuxA", "AXAuxB", "AXAuxC",
                      "AXCompressor", "AXStudio", "AXStereoPCM16", "AXSurround32",
                      "AXRemotePCM16", "AXDramContext", "AXFirmware"};
const NativeDSPMemoryEncoding encodings[] = {
    NativeDSPMemoryEncoding::NativeU16, NativeDSPMemoryEncoding::AXParameterBlocks,
    NativeDSPMemoryEncoding::RawBytes, NativeDSPMemoryEncoding::NativeU32,
    NativeDSPMemoryEncoding::NativeU32, NativeDSPMemoryEncoding::NativeU32,
    NativeDSPMemoryEncoding::NativeU16, NativeDSPMemoryEncoding::AXStudio,
    NativeDSPMemoryEncoding::NativeU16, NativeDSPMemoryEncoding::NativeU32,
    NativeDSPMemoryEncoding::NativeU16, NativeDSPMemoryEncoding::RawBytes,
    NativeDSPMemoryEncoding::RawBytes};
std::uint64_t Hash(const unsigned char* bytes, std::size_t count) {
    std::uint64_t result = 14695981039346656037ull;
    for (std::size_t i = 0; i != count; ++i) result = (result ^ bytes[i]) * 1099511628211ull;
    return result;
}
struct MailReader {
    NativeDSPMailboxEndpoint endpoint;
    std::atomic<bool> stop{};
    std::vector<std::uint32_t> words;
    std::exception_ptr failure;
    std::thread worker;
    explicit MailReader(NativeDSPMailboxEndpoint e):endpoint(e),worker([this] {
        try {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!stop && std::chrono::steady_clock::now() < end) {
                const auto high = DSPBackendMailToHigh(endpoint);
                if (high & 0x8000) {
                    // Real hardware endpoint acknowledgement only. This is no
                    // firmware execution, boot/INIT reply or task completion.
                    const auto low = DSPBackendMailToLow(endpoint);
                    words.push_back((std::uint32_t(high & 0x7fff) << 16) | low);
                } else std::this_thread::yield();
            }
        } catch (...) { failure = std::current_exception(); }
    }) {}
    void Finish() { stop = true; worker.join(); if (failure) std::rethrow_exception(failure); }
    ~MailReader() { if (worker.joinable()) { stop = true; worker.join(); } }
};

void Run(int argc, char** argv) {
    Check(argc == 3 || argc == 4, "Expected actual source image and SHA256 identity");
    const bool expect_negative = argc == 4;
    Check(std::strlen(argv[2]) == 64, "Source identity must be real SHA256");
    const auto directory = std::filesystem::absolute("native-ax-output-data").string();
    std::filesystem::create_directories(directory);
    AuroraConfig config{};
    config.appName = "Original AX output ABI";
    config.userPath = config.cachePath = directory.c_str(); config.resourcesPath = ".";
    config.desiredBackend = BACKEND_NULL; config.windowWidth = 320; config.windowHeight = 240;
    config.windowPosX = config.windowPosY = -1; config.mem1Size = MEM1_DEFAULT_SIZE;
    config.mem2Size = 64u * 1024u * 1024u; config.logLevel = LOG_WARNING;
    Check(aurora_initialize(argc,argv,&config).window != nullptr, "Actual core SDK did not initialize");
    OSInit(); AIInit(nullptr); InitializeNativeInterruptController();
    const auto endpoint = AttachNativeDSPMEM1();
    const auto mailbox = AttachNativeDSPMailboxes();
    Owner image{std::filesystem::absolute(argv[1]).string(), argv[2]};
    loading = &image;
    image.initial = SDL_LoadObject(image.path.c_str());
    loading = nullptr;
    if (!image.initial) throw std::runtime_error(std::string("Actual source image load failed: ") + SDL_GetError());
    Check(image.reserved && image.mappings.size() == 13 && image.leases.size() == 13,
          "Early loader endpoint did not retain all actual source-array owners");
    Check(retired == 0, "Recursive real image retention unexpectedly retired source statics");
    auto snapshot = Load<decltype(&charged_ax_output_snapshot)>(image.initial,"charged_ax_output_snapshot");
    ChargedAXOutputMemorySnapshot after{}; snapshot(&after);
    Check(after.memory_initialized == 1 && after.standard_address && after.virtual_address,
          "Genuine eager source pools did not lazily initialize original arenas");
    Check(after.standard_allocations == 0 && after.virtual_allocations > 0,
          "Actual function-pool backing no longer has its original allocator selection");
    Check(Load<decltype(&charged_ax_output_pool_owner_check)>(image.initial,"charged_ax_output_pool_owner_check")() == 3,
          "Actual logical16/32/64 pools lost genuine MEM2 source ownership");
    const auto slot_checks = Load<unsigned(*)()>(image.initial,"charged_ax_output_slot_stride_check")();
    Check(slot_checks != 0, "Actual source slot-stride qualifier did not execute");
    std::cout << "Original slot stride: " << slot_checks << " source/lifetime checks\n";
    const auto source_high = OSCachedToPhysical(reinterpret_cast<void*>(after.standard_address)) + after.standard_bytes;
    for (const auto& mapping : image.mappings)
        Check(source_high <= mapping.physical_address, "Source-captured MEM1 overlaps reserved AX device backing");
    Check(image.storage[8].bytes == 3 * 384 && image.storage[9].bytes == 768 &&
          image.storage[10].bytes == 4 * 18 * 10 * 2 && image.storage[11].bytes == 64,
          "Original AX output/context geometry changed");
    Check(image.task.bytes == sizeof(DSPTask) && image.task.bytes != 80,
          "CPU-only native DSPTask was forced into serialized Wii shape");
    Throws([&] { (void)ChargedDSPTaskMemoryWord(image.task.address,image.task.bytes,0); },
           "Unregistered CPU-only task gained a fake DSP bus address");
    alignas(32) unsigned char late[32]{};
    OSNativeStaticMemoryOwner late_owner{image.identity.c_str(),"LateAfterSourceCapture",late,sizeof(late),TRUE,
                                       &image,Owner::Retain,Owner::Release};
    Throws([&] { (void)OSNativeRegisterStaticMemory(&late_owner); },
           "Late static reservation overlapped already-captured source backing");
    Throws([&] { OSNativeReleaseStaticMemory(image.mappings[8]); },
           "Live output device pin did not retain its genuine source image");

    // Explicit bounded hardware diagnostic: source AXOut/AXInit are NOT run.
    // Initialize only the actual individual command/PB/allocator providers,
    // then call the real CPU NewFrame method with zero-voice source state.
    // The endpoint below only acknowledges genuine source request words.
    for (const char* name : {"__AXAllocInit","__AXVPBInit","__AXSPBInit","__AXAuxInit","__AXClInit"})
        Load<void(*)()>(image.initial,name)();
    auto source_ready = Load<decltype(&DSPCheckInit)>(image.initial,"DSPCheckInit");
    Check(!source_ready(), "Fixture invented DSP initialized state");
    auto frame = Load<decltype(&__AXOutNewFrame)>(image.initial,"__AXOutNewFrame");
    MailReader reader(mailbox);
    bool rejected = false;
    try {
        for (unsigned n = 0; n != (expect_negative ? 1u : 4u); ++n) {
            Check(frame() == 96 * 3797, "Original elapsed-cycle calculation changed without running DMA");
            Check(AIGetDMAStartAddr() == reinterpret_cast<uintptr_t>(image.storage[8].address) + ((n + 1) & 1) * 384,
                  "Original alternating output selection truncated pointer or changed ring order");
            Check(AIGetDMALength() == 384, "Original source-selected DMA sample count changed");
        }
    } catch (const std::exception& error) {
        if (!expect_negative) throw;
        rejected = std::strstr(error.what(), "32-bit") != nullptr || std::strstr(error.what(), "mail") != nullptr;
        std::cout << "Retained preimage source-mail failure: " << error.what() << '\n';
    }
    reader.Finish();
    if (expect_negative) Check(rejected, "Original full-pointer DSP mail did not decisively fail checked hardware word transport");
    else {
        Check(reader.words.size() == 8, "Actual source frame transport omitted command-size/address requests");
        for (unsigned n = 0; n != 4; ++n) {
            Check(reader.words[2*n] == 0x3abe0080, "Actual source command-size hardware mailbox changed");
            Check(reader.words[2*n+1] == image.mappings[0].physical_address + (n & 1) * 128,
                  "AXOut source list word lost original double-buffer physical address");
        }
        std::array<unsigned char,3*384> wire{};
        for (unsigned i = 0; i != wire.size()/2; ++i) {
            const auto sample = static_cast<std::uint16_t>(0x1234 + i * 37);
            wire[2*i] = sample >> 8; wire[2*i+1] = sample;
        }
        DSPBackendWriteMemory(endpoint,image.mappings[8].physical_address,wire.data(),wire.size());
        std::array<unsigned char,3*384> roundtrip{};
        DSPBackendReadMemory(endpoint,image.mappings[8].physical_address,roundtrip.data(),roundtrip.size());
        Check(roundtrip == wire, "DSP output PCM16 roundtrip changed sample/channel order");
        const auto* native = static_cast<const unsigned char*>(image.storage[8].address);
        for (unsigned i = 0; i != wire.size()/2; ++i) {
            std::uint16_t value{}; std::memcpy(&value,native+2*i,2);
            Check(value == static_cast<std::uint16_t>(0x1234 + i*37), "AX u32 array treated PCM pairs as numeric32 cells");
        }
        AISetDSPSampleRate(AI_SAMPLERATE_32KHZ);
        AIStartDMA();
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(90);
        while (std::chrono::steady_clock::now() < end) {
            ServiceNativeAI(); std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const auto status = GetNativeAIStatus();
        Check(status.submitted_blocks && status.consumed_blocks,
              "Actual SDL hardware did not consume source-selected AX output PCM");
        Check(status.last_input_hash == Hash(native,384),
              "AI transfer did not preserve the actual nonzero native source PCM cells");
        Check(status.maps_right_left_to_left_right, "Actual AI channel mapping differs from Wii right/left storage");
        Check(!source_ready(), "Transport probe fabricated DSP readiness after consuming PCM");
    }
    AIStopDMA(); ShutdownNativeAI();
    for (auto it=image.pins.rbegin(); it!=image.pins.rend(); ++it) ReleaseNativeDSPMemory(*it);
    image.pins.clear();
    DetachNativeDSPMailboxes(); ShutdownNativeInterruptController(); DetachNativeDSPMEM1();
    for (auto it=image.mappings.rbegin(); it!=image.mappings.rend(); ++it) OSNativeReleaseStaticMemory(*it);
    image.mappings.clear();
    Check(image.leases.empty() && retired == 0, "Source retired before real DMA/device-owner release");
    SDL_UnloadObject(image.initial); image.initial = nullptr;
    Check(retired == 1, "Real source image did not retire after its final actual loader handle");
    aurora_shutdown();
    std::cout << "Original AX output ABI: " << checks << " checks; pre-static13 actual source spans; "
              << "AX/DSP init, prior callback, firmware mixing and full-game loader remain unqualified\n";
}
}

extern "C" void ChargedAXOutputReserve311(const ChargedAXStorage* storage, uint32_t count,
                                         ChargedAXStorage task, ChargedAXOutputMemorySnapshot before) {
    Check(loading && !loading->reserved && count == 13, "Unexpected pre-static image reservation endpoint");
    auto& image = *loading;
    Check(before.memory_initialized == 0 && before.standard_address == 0 && before.virtual_address == 0,
          "Source pools already captured arenas before the platform-only reservation");
    image.early = before; image.task = task;
    std::copy(storage,storage+13,image.storage.begin());
    for (unsigned i = 0; i != count; ++i) {
        Check(storage[i].address && storage[i].bytes && reinterpret_cast<uintptr_t>(storage[i].address) > 0xffffffffULL,
              "Actual source backing did not exercise complete high native addresses");
        const bool writable = i != 6 && i != 7 && i != 12;
        OSNativeStaticMemoryOwner owner{image.identity.c_str(),names[i],storage[i].address,storage[i].bytes,
                                       writable ? TRUE : FALSE,&image,Owner::Retain,Owner::Release};
        const auto before_high = reinterpret_cast<uintptr_t>(OSGetArenaHi());
        const auto bank_start = reinterpret_cast<uintptr_t>(OSPhysicalToCached(0));
        image.mappings.push_back(OSNativeRegisterStaticMemory(&owner));
        Check(image.mappings.back().physical_address == before_high - bank_start - ((storage[i].bytes + 31u) & ~31u),
              "Actual pre-static bus reservation differs from independent aligned extent oracle");
        image.pins.push_back(PinNativeDSPMemory(storage[i].address,storage[i].bytes,writable,encodings[i]));
        Check(OSPhysicalToCached(image.mappings.back().physical_address) == storage[i].address,
              "Pre-static device address inverse did not retain its actual source array");
    }
    image.reserved = true;
}
extern "C" void ChargedAXOutputRetired311() { ++retired; }

int main(int argc, char** argv) {
    try { Run(argc,argv); return 0; }
    catch (const std::exception& error) { std::cerr << "Original AX output ABI: " << error.what() << '\n'; return 1; }
}
