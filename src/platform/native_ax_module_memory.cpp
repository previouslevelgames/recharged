#if defined(MSCHARGED_GAME_MODULE)
#error Native AX image ownership must use the host CRT outside game operators
#endif
#include "platform/native_ax_module_memory.h"
#include "platform/ai.h"
#include "platform/native_module_loader.h"
#include "platform/path.h"
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <array>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace mscharged::platform {
namespace {
void* armed_owner{};
std::recursive_mutex& LoaderExclusion() { static std::recursive_mutex mutex; return mutex; }
const char* const Names[CHARGED_AX_HBM_STORAGE_COUNT] = {"AXCommandLists", "AXPB", "AXITD", "AXAuxA", "AXAuxB", "AXAuxC",
    "AXCompressor", "AXStudio", "AXStereoPCM16", "AXSurround32", "AXRemotePCM16", "AXDramContext", "AXFirmware", "HBMZeroBuffer"};
constexpr std::uint32_t Sizes[CHARGED_AX_HBM_STORAGE_COUNT] = {256,30720,6144,4608,4608,3456,4032,120,1152,768,1440,64,8192,256};
constexpr NativeDSPMemoryEncoding Encodings[CHARGED_AX_HBM_STORAGE_COUNT] = {NativeDSPMemoryEncoding::NativeU16,
    NativeDSPMemoryEncoding::AXParameterBlocks, NativeDSPMemoryEncoding::RawBytes,
    NativeDSPMemoryEncoding::NativeU32, NativeDSPMemoryEncoding::NativeU32, NativeDSPMemoryEncoding::NativeU32,
    NativeDSPMemoryEncoding::NativeU16, NativeDSPMemoryEncoding::AXStudio, NativeDSPMemoryEncoding::NativeU16,
    NativeDSPMemoryEncoding::NativeU32, NativeDSPMemoryEncoding::NativeU16,
    NativeDSPMemoryEncoding::RawBytes, NativeDSPMemoryEncoding::RawBytes, NativeDSPMemoryEncoding::RawBytes};

}
struct NativeAXModuleMemory::State {
    std::unique_ptr<NativeModuleFile> image_file;
    std::thread::id owner{std::this_thread::get_id()};
    std::array<OSNativeStaticMemory,CHARGED_AX_HBM_STORAGE_COUNT> mappings{};
    std::array<NativeDSPMemoryPin,CHARGED_AX_HBM_STORAGE_COUNT> pins{};
    std::array<void*,CHARGED_AX_HBM_STORAGE_COUNT> retained{};
    std::array<ChargedAXStorage,CHARGED_AX_HBM_STORAGE_COUNT> storage{};
    std::uint32_t mapping_count{}, pin_count{}, lease_count{};
    std::uintptr_t image_base{};
    NativeDSPMemoryEndpoint endpoint{};
    ChargedAXModuleArenaSnapshot before{};
    ChargedAXModuleArenaObserver observe{};
    bool registered{}, loaded{}, retired{}, reserving{};
    void RequireOwner() const {
        if(owner!=std::this_thread::get_id()) throw std::logic_error("AX module ownership requires actual loader owner thread");
    }
    void RequireFile() const { image_file->RequireUnchanged(); }
    static BOOL Retain(void* context) noexcept {
        auto& self=*static_cast<State*>(context);
        if(self.owner!=std::this_thread::get_id() || self.lease_count==self.retained.size()) return FALSE;
        // Retain the same actual mapped image. ELF/Mach-O never upgrade LAZY;
        // PE only references an already loaded DLL, never a substitute image.
        void* image=RetainNativeModuleImage(self.image_file->Path(),self.image_base);
        if(!image) return FALSE;
        self.retained[self.lease_count++]=image;
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& self=*static_cast<State*>(context);
        if(!self.lease_count || self.owner!=std::this_thread::get_id()) std::terminate();
        void* image=self.retained[--self.lease_count];self.retained[self.lease_count]=nullptr;
        if(!ReleaseNativeModule(image)) std::terminate();
    }
    void Release() {
        while(pin_count) ReleaseNativeDSPMemory(pins[--pin_count]);
        while(mapping_count) OSNativeReleaseStaticMemory(mappings[--mapping_count]);
    }
};

NativeAXModuleMemory::NativeAXModuleMemory(const char* module_path)
    :NativeAXModuleMemory(module_path ? mscharged::PathFromUtf8(module_path) : std::filesystem::path{}) {}
NativeAXModuleMemory::NativeAXModuleMemory(const std::filesystem::path& module_path):state_(new State) {
    std::lock_guard lock(LoaderExclusion());
    if(armed_owner) throw std::logic_error("Another actual AX module load is already armed");
    if(__OSCurrHeap!=-1) throw std::logic_error("AX image load must precede original source reserve-heap capture");
    if(module_path.empty()) throw std::invalid_argument("AX module load requires its actual image path");
    auto& s=*state_;
    s.image_file=std::make_unique<NativeModuleFile>(module_path);
    s.endpoint=AttachNativeDSPMEM1();
    armed_owner=&s;
}
NativeAXModuleMemory::~NativeAXModuleMemory() {
    std::lock_guard lock(LoaderExclusion());
    auto& s=*state_;
    // Never implicitly free device/source images before an explicit drain.
    if(s.pin_count || s.mapping_count || s.lease_count) std::terminate();
    if(armed_owner==&s) armed_owner=nullptr;
}
void NativeAXModuleMemory::ConfirmLoaded(void* actual_loader_handle) {
    std::lock_guard lock(LoaderExclusion());
    auto& s=*state_;s.RequireOwner();s.RequireFile();
    NativeModuleImage image;
    if(!QueryNativeModuleHandle(actual_loader_handle,image) || !s.registered ||
        !s.image_file->OwnsImage(image) || image.base!=s.image_base)
        throw std::logic_error("Actual module load did not register its own pre-static AX spans");
    s.loaded=true;if(armed_owner==&s)armed_owner=nullptr;
}
NativeAXModuleMemoryStatus NativeAXModuleMemory::Status() const {
    std::lock_guard lock(LoaderExclusion());
    auto& s=*state_;s.RequireOwner();ChargedAXModuleArenaSnapshot after{};
    if(s.observe && !s.retired) s.observe(&after);
    std::uint32_t bytes=0;for(unsigned i=0;i<s.mapping_count;++i)bytes+=(s.mappings[i].bytes+31)&~31u;
    return {armed_owner==&s,s.registered,s.loaded,s.retired,s.mapping_count,bytes,s.image_base,s.before,after};
}
NativeDSPMemoryEndpoint NativeAXModuleMemory::Endpoint() const { state_->RequireOwner();return state_->endpoint; }
std::uint32_t NativeAXModuleMemory::PhysicalAddress(unsigned source_span) const {
    auto& s=*state_;s.RequireOwner();
    if(!s.loaded || s.retired || source_span>=s.mapping_count)
        throw std::out_of_range("AX source static word requires a live loaded span");
    return s.mappings[source_span].physical_address;
}
bool NativeAXModuleMemory::OwnsReadableImageExtent(const void* address, std::size_t bytes) const {
    std::lock_guard lock(LoaderExclusion());
    auto& s=*state_;s.RequireOwner();
    if (!s.loaded || s.retired) return false;
    s.RequireFile();
    return NativeModuleOwnsReadableExtent(s.image_base,address,bytes);
}
void NativeAXModuleMemory::ReleaseAfterDeviceDrain() {
    auto& s=*state_;s.RequireOwner();
    if(!s.loaded || s.retired) throw std::logic_error("AX module release requires its live loaded owner");
    const auto ai=GetNativeAIStatus();
    if(ai.running || ai.callback_active || ai.retained_blocks || ai.queued_input_bytes)
        throw std::logic_error("Actual audio DMA/output must drain before AX source pins retire");
    // Source halt/job drain is the caller's mandatory contract. The checked
    // pin release synchronizes real per-transfer readers, not whole jobs.
    s.Release();s.observe=nullptr;s.retired=true;
}
}

extern "C" void ChargedNativeAXReserveModuleStorage(const ChargedAXStorage* storage,
    uint32_t count,ChargedAXStorage cpu_task,ChargedAXModuleArenaSnapshot before,
    ChargedAXModuleArenaObserver observe) {
    using namespace mscharged::platform;
    std::lock_guard lock(LoaderExclusion());
    auto* s=static_cast<NativeAXModuleMemory::State*>(armed_owner);
    if(!s) throw std::logic_error("Original module AX statics require an armed host before dlopen");
    s->RequireOwner();s->RequireFile();
    if(s->reserving || s->registered || (count!=CHARGED_AX_BASE_STORAGE_COUNT && count!=CHARGED_AX_HBM_STORAGE_COUNT) || !storage || !observe ||
        before.memory_initialized || before.standard_address || before.virtual_address)
        throw std::logic_error("AX reservation must precede all original source arena capture");
    if(!cpu_task.address || cpu_task.bytes!=120)
        throw std::invalid_argument("Actual CPU-only native DSPTask has an unexpected ABI");
    s->reserving=true;s->before=before;s->observe=observe;
    try {
        for(unsigned i=0;i<count;++i) {
            if(!storage[i].address || storage[i].bytes!=Sizes[i])
                throw std::invalid_argument("Actual AX source array does not match its reviewed native/wire extent");
            NativeModuleImage info;
            if(!QueryNativeModuleImage(storage[i].address,info) || !s->image_file->OwnsImage(info))
                throw std::invalid_argument("AX static array is not backed by the armed actual module");
            const auto base=info.base;
            if(i && base!=s->image_base) throw std::invalid_argument("AX static arrays belong to different images");
            s->image_base=base;s->storage[i]=storage[i];
            const bool writable=i!=6 && i!=7 && i!=12 && i!=13;
            OSNativeStaticMemoryOwner owner{s->image_file->Identity().c_str(),Names[i],storage[i].address,storage[i].bytes,
                writable?TRUE:FALSE,s,NativeAXModuleMemory::State::Retain,NativeAXModuleMemory::State::Release};
            s->mappings[i]=OSNativeRegisterStaticMemory(&owner);++s->mapping_count;
            s->pins[i]=PinNativeDSPMemory(storage[i].address,storage[i].bytes,writable,Encodings[i]);++s->pin_count;
        }
        NativeModuleImage task_info;
        if(!QueryNativeModuleImage(cpu_task.address,task_info) || task_info.base!=s->image_base)
            throw std::invalid_argument("Actual CPU-only DSPTask belongs to a different module owner");
        s->registered=true;s->reserving=false;
    } catch (...) {
        s->Release();s->observe=nullptr;s->reserving=false;throw;
    }
}
