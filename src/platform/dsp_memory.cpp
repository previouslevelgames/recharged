#include "platform/dsp_memory.h"
#include "platform/dsp_memory_abi.h"
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace mscharged::platform;
struct Span {
    std::uint64_t identity;
    std::uint32_t bytes;
    bool writable;
    std::uintptr_t native_address;
    std::uint64_t static_identity;
};
struct AddressSpace {
    std::mutex mutex;
    std::thread::id owner;
    std::uintptr_t start{};
    std::uint32_t bytes{};
    std::uint64_t generation{},next_identity{};
    bool attached{};
    std::map<std::uint32_t,Span> spans;
};
AddressSpace& State() { static AddressSpace state;return state; }
void RequireLive(const AddressSpace& state) {
    if (!state.attached || !OSGetArenaLo() || !OSGetArenaHi())
        throw std::logic_error("DSP memory endpoint has no live SDK MEM1 owner");
    if (reinterpret_cast<std::uintptr_t>(OSPhysicalToCached(0))!=state.start
        || OSGetPhysicalMemSize()!=state.bytes)
        throw std::logic_error("DSP memory endpoint no longer matches its SDK MEM1 backing");
}
void RequireOwner(const AddressSpace& state) {
    RequireLive(state);
    if (state.owner!=std::this_thread::get_id())
        throw std::logic_error("DSP memory registration/source transport requires the owner thread");
}
void RequireDevice(const AddressSpace& state,NativeDSPMemoryEndpoint endpoint) {
    RequireLive(state);
    if (endpoint.generation!=state.generation)
        throw std::logic_error("DSP memory device endpoint is stale");
}
std::uint32_t Offset(const AddressSpace& state,const void* address,std::size_t bytes) {
    const auto value=reinterpret_cast<std::uintptr_t>(address);
    if (value<state.start || value-state.start>=state.bytes)
        throw std::out_of_range("DSP task pointer is outside real MEM1; static/MEM2 domain is unavailable");
    const auto offset=value-state.start;
    if (bytes>state.bytes-offset)
        throw std::out_of_range("DSP memory extent crosses its actual MEM1 backing");
    // The checked range proves the canonical native conversion is representable.
    const auto physical=OSCachedToPhysical(const_cast<void*>(address));
    if (physical!=offset)
        throw std::logic_error("Canonical SDK physical address does not match its real backing");
    return physical;
}
const Span& Find(const AddressSpace& state,std::uint32_t address,std::size_t bytes,bool writing) {
    auto it=state.spans.upper_bound(address);
    if (it==state.spans.begin()) throw std::out_of_range("DSP address has no live pinned source backing");
    --it;
    const auto offset=static_cast<std::uint64_t>(address)-it->first;
    if (offset>=it->second.bytes || bytes>it->second.bytes-offset)
        throw std::out_of_range("DSP transfer exceeds its live pinned source extent");
    if (writing && !it->second.writable)
        throw std::invalid_argument("DSP transfer writes read-only pinned source backing");
    return it->second;
}
void* NativePointer(const AddressSpace& state,std::uint32_t physical,std::size_t bytes,bool writing) {
    const auto& span=Find(state,physical,bytes,writing);
    auto it=state.spans.upper_bound(physical);--it;
    const auto pointer=reinterpret_cast<void*>(span.native_address+physical-it->first);
    if (OSPhysicalToCached(physical)!=pointer)
        throw std::logic_error("DSP physical address no longer resolves to the retained source backing");
    return pointer;
}
std::uint32_t SharedPhysical(const void* address) {
    const auto physical=OSCachedToPhysical(const_cast<void*>(address));
    if (OSPhysicalToCached(physical)!=address)
        throw std::logic_error("Canonical SDK address conversion is not coherent");
    return physical;
}
NativeDSPMemoryPin Pin(AddressSpace& state,const void* address,std::size_t bytes,bool writable) {
    if (!bytes || bytes>std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("DSP source pin has no representable positive byte extent");
    std::uint32_t physical{};
    const auto retained=OSNativePinAddress(address,static_cast<std::uint32_t>(bytes),writable?TRUE:FALSE,&physical);
    try {
        const auto end=static_cast<std::uint64_t>(physical)+bytes;
        auto next=state.spans.lower_bound(physical);
        if (next!=state.spans.end() && next->first<end)
            throw std::invalid_argument("DSP source pin overlaps an existing live extent");
        if (next!=state.spans.begin()) {
            const auto previous=std::prev(next);
            if (static_cast<std::uint64_t>(previous->first)+previous->second.bytes>physical)
                throw std::invalid_argument("DSP source pin overlaps an existing live extent");
        }
        const auto identity=++state.next_identity;
        state.spans.emplace(physical,Span{identity,static_cast<std::uint32_t>(bytes),writable,
            reinterpret_cast<std::uintptr_t>(address),retained});
        return {state.generation,identity};
    } catch (...) {
        if (retained) OSNativeUnpinAddress(retained);
        throw;
    }
}
}
namespace mscharged::platform {
NativeDSPMemoryEndpoint AttachNativeDSPMEM1() {
    auto& state=State();std::lock_guard lock(state.mutex);
    if (state.attached) { RequireOwner(state);return {state.generation}; }
    if (!OSGetArenaLo() || !OSGetArenaHi())
        throw std::logic_error("DSP memory transport cannot invent an initialized MEM1 owner");
    const auto bytes=OSGetPhysicalMemSize();
    const auto start=reinterpret_cast<std::uintptr_t>(OSPhysicalToCached(0));
    if (!start || !bytes || bytes>std::numeric_limits<std::uintptr_t>::max()-start)
        throw std::logic_error("SDK MEM1 backing has an invalid native extent");
    state.start=start;state.bytes=bytes;state.owner=std::this_thread::get_id();
    ++state.generation;state.attached=true;
    return {state.generation};
}
void DetachNativeDSPMEM1() {
    auto& state=State();std::lock_guard lock(state.mutex);
    if (!state.attached) return;
    if (state.owner!=std::this_thread::get_id())
        throw std::logic_error("DSP memory detach requires its owner thread");
    for (const auto& [physical,span]:state.spans)
        if (span.static_identity) OSNativeUnpinAddress(span.static_identity);
    state.spans.clear();state.attached=false;state.owner={};state.start=0;state.bytes=0;
}
NativeDSPMemoryPin PinNativeDSPMEM1(const void* address,std::size_t bytes,bool writable) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireOwner(state);
    Offset(state,address,bytes); // Retain the original explicit MEM1-only contract.
    return Pin(state,address,bytes,writable);
}
NativeDSPMemoryPin PinNativeDSPMemory(const void* address,std::size_t bytes,bool writable) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireOwner(state);
    return Pin(state,address,bytes,writable);
}
void ReleaseNativeDSPMemory(NativeDSPMemoryPin pin) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireOwner(state);
    if (pin.generation!=state.generation)
        throw std::invalid_argument("DSP source pin belongs to an old memory endpoint");
    for (auto it=state.spans.begin();it!=state.spans.end();++it) {
        if (it->second.identity==pin.identity) {
            if (it->second.static_identity) OSNativeUnpinAddress(it->second.static_identity);
            state.spans.erase(it);return;
        }
    }
    throw std::invalid_argument("DSP source pin was already released or never existed");
}
void DSPBackendReadMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,void* destination,std::size_t bytes) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    if (!destination && bytes) throw std::invalid_argument("DSP read destination is null");
    auto* source=NativePointer(state,address,bytes,false);
    if (bytes) std::memcpy(destination,source,bytes);
}
void DSPBackendWriteMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,const void* source,std::size_t bytes) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    if (!source && bytes) throw std::invalid_argument("DSP write source is null");
    auto* destination=NativePointer(state,address,bytes,true);
    if (bytes) std::memcpy(destination,source,bytes);
}
}
extern "C" std::uint32_t ChargedDSPTaskMemoryWord(const void* address,std::uint32_t bytes,int writing) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireOwner(state);
    if (writing!=0 && writing!=1) throw std::invalid_argument("DSP task memory access direction is invalid");
    if (!address && !bytes) return 0; // Original empty task/context fields.
    const auto physical=SharedPhysical(address);
    const auto* native=NativePointer(state,physical,bytes,writing!=0);
    if (native!=address) throw std::logic_error("DSP source pointer differs from its pinned native owner");
    return physical;
}
