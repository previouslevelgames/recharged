#include "platform/dsp_mailbox.h"
#include "platform/dsp_mailbox_abi.h"
#include "platform/interrupts.h"
#include "platform/interrupt_controller.h"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
struct MailCell {
    std::uint16_t payload_high{},payload_low{};
    bool full{};
    std::uint16_t High() const { return payload_high | (full ? 0x8000u : 0u); }
    std::uint16_t Low() { full=false;return payload_low; }
    void WriteHigh(std::uint16_t value) { payload_high=value&0x7fffu;full=false; }
    void WriteLow(std::uint16_t value) { payload_low=value;full=true; }
};
struct Mailboxes {
    std::mutex mutex;
    std::thread::id owner{};
    NativeInterruptSource interrupt{};
    bool connected{};
    std::uint64_t generation{};
    MailCell to_dsp,from_dsp;
};
Mailboxes& State() { static Mailboxes state;return state; }
void RequireCPU(const Mailboxes& state) {
    if (!state.connected) throw std::logic_error("DSP mailbox transport has no connected device endpoint");
    if (state.owner!=std::this_thread::get_id())
        throw std::logic_error("DSP CPU mailbox access requires the game owner thread");
}
void RequireBackend(const Mailboxes& state,NativeDSPMailboxEndpoint endpoint) {
    if (!state.connected || state.generation!=endpoint.generation)
        throw std::logic_error("DSP mailbox device endpoint is stale or detached");
}
}
extern "C" std::uint16_t ChargedDSPMailToHigh() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    RequireCPU(state);return state.to_dsp.High();
}
extern "C" std::uint16_t ChargedDSPMailFromHigh() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    RequireCPU(state);return state.from_dsp.High();
}
extern "C" std::uint16_t ChargedDSPMailFromLow() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    RequireCPU(state);return state.from_dsp.Low();
}
extern "C" void ChargedDSPMailToWriteHigh(std::uint16_t value) {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    RequireCPU(state);state.to_dsp.WriteHigh(value);
}
extern "C" void ChargedDSPMailToWriteLow(std::uint16_t value) {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    RequireCPU(state);state.to_dsp.WriteLow(value);
}
extern "C" std::uint32_t ChargedDSPRequireMailWord(std::uintptr_t value) {
    if (value>std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("DSP mail needs a genuine 32-bit bus address; native pointer mapping is unavailable");
    return static_cast<std::uint32_t>(value);
}
namespace mscharged::platform {
NativeDSPMailboxEndpoint AttachNativeDSPMailboxes() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    if (state.connected) {RequireCPU(state);return {state.generation};}
    state.interrupt=GetNativeInterruptSource(__OS_INTERRUPT_DSP_DSP);
    state.owner=std::this_thread::get_id();
    state.to_dsp={};state.from_dsp={};++state.generation;state.connected=true;
    return {state.generation};
}
void DetachNativeDSPMailboxes() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    if (!state.connected) return;
    RequireCPU(state);
    SetNativeInterruptPending(state.interrupt,false);
    state.connected=false;state.owner={};state.to_dsp={};state.from_dsp={};
}
NativeDSPMailboxStatus GetNativeDSPMailboxStatus() {
    NativeInterruptGuard exclusion;auto& state=State();std::lock_guard lock(state.mutex);
    return {state.connected,state.to_dsp.full,state.from_dsp.full,state.generation};
}
std::uint16_t DSPBackendMailToHigh(NativeDSPMailboxEndpoint endpoint) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireBackend(state,endpoint);
    return state.to_dsp.High();
}
std::uint16_t DSPBackendMailToLow(NativeDSPMailboxEndpoint endpoint) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireBackend(state,endpoint);
    return state.to_dsp.Low();
}
void DSPBackendMailFromWriteHigh(NativeDSPMailboxEndpoint endpoint,std::uint16_t value) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireBackend(state,endpoint);
    state.from_dsp.WriteHigh(value);
}
void DSPBackendMailFromWriteLow(NativeDSPMailboxEndpoint endpoint,std::uint16_t value) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireBackend(state,endpoint);
    state.from_dsp.WriteLow(value);
}
bool DSPBackendSetInterrupt(NativeDSPMailboxEndpoint endpoint,bool asserted) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireBackend(state,endpoint);
    return SetNativeInterruptPending(state.interrupt,asserted);
}
} // namespace mscharged::platform
