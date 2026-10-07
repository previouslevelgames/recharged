#include "platform/os_boot_environment.h"
#include "platform/os_boot_environment_abi.h"
#include <dolphin/os.h>
#include <mutex>
#include <stdexcept>
#include <thread>

// Original OS.c owns BOOL __OSInIPL in BSS. Native OSInit is the platform
// replacement and owns the same32-bit cell. No source DSP/audio flag is here.
extern "C" {int __OSInIPL;}
static_assert(sizeof(__OSInIPL)==4);
namespace {
struct State {
    std::mutex mutex;
    bool configured{},in_ipl{};
    std::thread::id owner{};
    std::uint64_t generation{},leases{};
};
State& Environment() {static State state;return state;}
void Require(const State& state) {
    if(!state.configured||state.owner!=std::this_thread::get_id())
        throw std::logic_error("native OS boot environment is unconfigured, retired or foreign");
    if(__OSInIPL!=int(state.in_ipl))
        throw std::logic_error("native OS boot environment cell was changed outside its owner");
    if(!OSGetArenaLo()||!OSGetArenaHi()||!OSGetPhysicalMemSize())
        throw std::logic_error("native OS boot environment outlived its actual SDK arenas");
}
}
namespace mscharged::platform {
void ConfigureNativeOSBootEnvironment(NativeOSBootEnvironment mode) {
    auto& state=Environment();std::lock_guard lock(state.mutex);
    if(state.configured||state.leases)
        throw std::logic_error("native OS boot environment is already configured or retained");
    if(mode!=NativeOSBootEnvironment::DesktopApplication&&mode!=NativeOSBootEnvironment::IPLDiagnostic)
        throw std::invalid_argument("native OS boot environment mode is unsupported");
    if(!OSGetArenaLo()||!OSGetArenaHi()||!OSGetPhysicalMemSize())
        throw std::logic_error("native OS boot environment needs actual SDK platform initialization");
    state.owner=std::this_thread::get_id();state.in_ipl=mode==NativeOSBootEnvironment::IPLDiagnostic;
    __OSInIPL=int(state.in_ipl);++state.generation;state.configured=true;
}
void RetireNativeOSBootEnvironment() {
    auto& state=Environment();std::lock_guard lock(state.mutex);Require(state);
    if(state.leases)throw std::logic_error("native OS boot environment still has live source/device leases");
    state.configured=false;state.owner={};
}
NativeOSBootEnvironmentLease::NativeOSBootEnvironmentLease() {
    auto& state=Environment();std::lock_guard lock(state.mutex);Require(state);
    generation_=state.generation;++state.leases;
}
NativeOSBootEnvironmentLease::~NativeOSBootEnvironmentLease() {
    if(generation_) {try {Close();}catch(...) {std::terminate();}}
}
void NativeOSBootEnvironmentLease::RequireLive() const {
    auto& state=Environment();std::lock_guard lock(state.mutex);Require(state);
    if(!generation_||state.generation!=generation_)
        throw std::logic_error("native OS boot environment lease is stale or retired");
}
void NativeOSBootEnvironmentLease::Close() {
    if(!generation_)return;
    auto& state=Environment();std::lock_guard lock(state.mutex);Require(state);
    if(state.generation!=generation_||!state.leases)
        throw std::logic_error("native OS boot environment lease has another lifetime");
    --state.leases;generation_=0;
}
}
extern "C" int ChargedNativeOSInIPL() {
    auto& state=Environment();std::lock_guard lock(state.mutex);Require(state);return __OSInIPL;
}
