#if defined(MSCHARGED_GAME_MODULE)
#error Native AX image ownership must use the host CRT outside game operators
#endif
#include "platform/native_ax_module_memory.h"
#include "platform/ai.h"
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <link.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace mscharged::platform {
namespace {
void* armed_owner{};
std::recursive_mutex& LoaderExclusion() { static std::recursive_mutex mutex; return mutex; }
const char* const Names[13] = {"AXCommandLists", "AXPB", "AXITD", "AXAuxA", "AXAuxB", "AXAuxC",
    "AXCompressor", "AXStudio", "AXStereoPCM16", "AXSurround32", "AXRemotePCM16", "AXDramContext", "AXFirmware"};
constexpr std::uint32_t Sizes[13] = {256,30720,6144,4608,4608,3456,4032,120,1152,768,1440,64,8192};
constexpr NativeDSPMemoryEncoding Encodings[13] = {NativeDSPMemoryEncoding::NativeU16,
    NativeDSPMemoryEncoding::AXParameterBlocks, NativeDSPMemoryEncoding::RawBytes,
    NativeDSPMemoryEncoding::NativeU32, NativeDSPMemoryEncoding::NativeU32, NativeDSPMemoryEncoding::NativeU32,
    NativeDSPMemoryEncoding::NativeU16, NativeDSPMemoryEncoding::AXStudio, NativeDSPMemoryEncoding::NativeU16,
    NativeDSPMemoryEncoding::NativeU32, NativeDSPMemoryEncoding::NativeU16,
    NativeDSPMemoryEncoding::RawBytes, NativeDSPMemoryEncoding::RawBytes};
bool SameFile(const struct stat& a, const struct stat& b) {
    return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size &&
        a.st_mtim.tv_sec==b.st_mtim.tv_sec && a.st_mtim.tv_nsec==b.st_mtim.tv_nsec;
}
}
struct NativeAXModuleMemory::State {
    std::string path, identity;
    int file{-1}; struct stat file_state{};
    std::thread::id owner{std::this_thread::get_id()};
    std::array<OSNativeStaticMemory,13> mappings{};
    std::array<NativeDSPMemoryPin,13> pins{};
    std::array<void*,13> retained{};
    std::array<ChargedAXStorage,13> storage{};
    std::uint32_t mapping_count{}, pin_count{}, lease_count{};
    std::uintptr_t image_base{};
    NativeDSPMemoryEndpoint endpoint{};
    ChargedAXModuleArenaSnapshot before{};
    ChargedAXModuleArenaObserver observe{};
    bool registered{}, loaded{}, retired{}, reserving{};
    void RequireOwner() const {
        if(owner!=std::this_thread::get_id()) throw std::logic_error("AX module ownership requires actual loader owner thread");
    }
    void RequireFile() const {
        struct stat now{}, named{};
        if(fstat(file,&now) || stat(path.c_str(),&named) || !SameFile(file_state,now) || !SameFile(now,named))
            throw std::logic_error("AX source image identity changed during actual loader ownership");
    }
    static BOOL Retain(void* context) noexcept {
        auto& self=*static_cast<State*>(context);
        if(self.owner!=std::this_thread::get_id() || self.lease_count==13) return FALSE;
        // NOLOAD retains the real currently mapped image. LAZY preserves its
        // unresolved/unexecuted source function boundary; never upgrade NOW.
        void* image=dlopen(self.path.c_str(),RTLD_NOLOAD|RTLD_LAZY|RTLD_LOCAL);
        if(!image) return FALSE;
        self.retained[self.lease_count++]=image;
        return TRUE;
    }
    static void Release(void* context) noexcept {
        auto& self=*static_cast<State*>(context);
        if(!self.lease_count || self.owner!=std::this_thread::get_id()) std::terminate();
        void* image=self.retained[--self.lease_count];self.retained[self.lease_count]=nullptr;
        if(dlclose(image)) std::terminate();
    }
    void Release() {
        while(pin_count) ReleaseNativeDSPMemory(pins[--pin_count]);
        while(mapping_count) OSNativeReleaseStaticMemory(mappings[--mapping_count]);
    }
    ~State() { if(file!=-1) close(file); }
};

NativeAXModuleMemory::NativeAXModuleMemory(const char* module_path):state_(new State) {
    std::lock_guard lock(LoaderExclusion());
    if(armed_owner) throw std::logic_error("Another actual AX module load is already armed");
    if(__OSCurrHeap!=-1) throw std::logic_error("AX image load must precede original source reserve-heap capture");
    if(!module_path || !*module_path) throw std::invalid_argument("AX module load requires its actual image path");
    auto& s=*state_;
    s.path=std::filesystem::canonical(module_path).string();
    s.file=open(s.path.c_str(),O_RDONLY|O_CLOEXEC);
    if(s.file==-1 || fstat(s.file,&s.file_state) || s.file_state.st_size<=0)
        throw std::invalid_argument("AX module image has no live regular source file");
    if(!S_ISREG(s.file_state.st_mode)) throw std::invalid_argument("AX module source is not a regular image");
    // Persistent actual file incarnation plus full-byte fingerprint. This is
    // ownership identity, not a build signature or security digest.
    std::uint64_t fingerprint=14695981039346656037ull;
    unsigned char bytes[4096];ssize_t count;
    while((count=read(s.file,bytes,sizeof(bytes)))>0)
        for(ssize_t n=0;n<count;++n) fingerprint=(fingerprint^bytes[n])*1099511628211ull;
    if(count<0) throw std::runtime_error("AX source image identity read failed");
    s.RequireFile();
    char suffix[160];std::snprintf(suffix,sizeof(suffix),":%llu:%llu:%llu:%lld:%ld:%016llx",
        static_cast<unsigned long long>(s.file_state.st_dev),static_cast<unsigned long long>(s.file_state.st_ino),
        static_cast<unsigned long long>(s.file_state.st_size),static_cast<long long>(s.file_state.st_mtim.tv_sec),
        s.file_state.st_mtim.tv_nsec,static_cast<unsigned long long>(fingerprint));
    s.identity=s.path+suffix;
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
    link_map* map{};
    if(!actual_loader_handle || dlinfo(actual_loader_handle,RTLD_DI_LINKMAP,&map) || !map ||
        !s.registered || reinterpret_cast<std::uintptr_t>(map->l_addr)!=s.image_base)
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
    if(s->reserving || s->registered || count!=13 || !storage || !observe ||
        before.memory_initialized || before.standard_address || before.virtual_address)
        throw std::logic_error("AX reservation must precede all original source arena capture");
    if(!cpu_task.address || cpu_task.bytes!=120)
        throw std::invalid_argument("Actual CPU-only native DSPTask has an unexpected ABI");
    s->reserving=true;s->before=before;s->observe=observe;
    try {
        for(unsigned i=0;i<13;++i) {
            if(!storage[i].address || storage[i].bytes!=Sizes[i])
                throw std::invalid_argument("Actual AX source array does not match its reviewed native/wire extent");
            Dl_info info{};
            if(!dladdr(storage[i].address,&info) || !info.dli_fbase || !info.dli_fname ||
                std::filesystem::canonical(info.dli_fname).string()!=s->path)
                throw std::invalid_argument("AX static array is not backed by the armed actual module");
            const auto base=reinterpret_cast<std::uintptr_t>(info.dli_fbase);
            if(i && base!=s->image_base) throw std::invalid_argument("AX static arrays belong to different images");
            s->image_base=base;s->storage[i]=storage[i];
            const bool writable=i!=6 && i!=7 && i!=12;
            OSNativeStaticMemoryOwner owner{s->identity.c_str(),Names[i],storage[i].address,storage[i].bytes,
                writable?TRUE:FALSE,s,NativeAXModuleMemory::State::Retain,NativeAXModuleMemory::State::Release};
            s->mappings[i]=OSNativeRegisterStaticMemory(&owner);++s->mapping_count;
            s->pins[i]=PinNativeDSPMemory(storage[i].address,storage[i].bytes,writable,Encodings[i]);++s->pin_count;
        }
        Dl_info task_info{};
        if(!dladdr(cpu_task.address,&task_info) ||
            reinterpret_cast<std::uintptr_t>(task_info.dli_fbase)!=s->image_base)
            throw std::invalid_argument("Actual CPU-only DSPTask belongs to a different module owner");
        s->registered=true;s->reserving=false;
    } catch (...) {
        s->Release();s->observe=nullptr;s->reserving=false;throw;
    }
}
