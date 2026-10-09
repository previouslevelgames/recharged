#include "platform/dsp_memory.h"
#include "platform/dsp_memory_abi.h"
#include <dolphin/os.h>
#include <dolphin/os/OSNativeMemory.h>
#include <cstdio>
#include <algorithm>
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
    NativeDSPMemoryEncoding encoding;
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
using SpanIterator=std::map<std::uint32_t,Span>::const_iterator;
SpanIterator FindSpan(const AddressSpace& state,std::uint32_t address,std::size_t bytes,bool writing) {
    auto it=state.spans.upper_bound(address);
    if (it==state.spans.begin()) throw std::out_of_range("DSP address has no live pinned source backing");
    --it;
    const auto offset=static_cast<std::uint64_t>(address)-it->first;
    if (offset>=it->second.bytes || bytes>it->second.bytes-offset) {
        char detail[160];
        std::snprintf(detail,sizeof(detail),
                      "DSP transfer exceeds its live pinned source extent (address=0x%08x bytes=%zu span=0x%08x+0x%x)",
                      address,bytes,it->first,it->second.bytes);
        throw std::out_of_range(detail);
    }
    if (writing && !it->second.writable)
        throw std::invalid_argument("DSP transfer writes read-only pinned source backing");
    return it;
}
void* SpanPointer(SpanIterator it,std::uint32_t physical) {
    const auto pointer=reinterpret_cast<void*>(it->second.native_address+physical-it->first);
    if (OSPhysicalToCached(physical)!=pointer)
        throw std::logic_error("DSP physical address no longer resolves to the retained source backing");
    return pointer;
}
void* NativePointer(const AddressSpace& state,std::uint32_t physical,std::size_t bytes,bool writing) {
    return SpanPointer(FindSpan(state,physical,bytes,writing),physical);
}
std::uint32_t SharedPhysical(const void* address) {
    const auto physical=OSCachedToPhysical(const_cast<void*>(address));
    if (OSPhysicalToCached(physical)!=address)
        throw std::logic_error("Canonical SDK address conversion is not coherent");
    return physical;
}
void RequireEncoding(NativeDSPMemoryEncoding encoding,std::size_t bytes) {
    switch (encoding) {
    case NativeDSPMemoryEncoding::RawBytes: return;
    case NativeDSPMemoryEncoding::NativeU16:
        if (bytes%2==0) return;
        break;
    case NativeDSPMemoryEncoding::NativeU32:
        if (bytes%4==0) return;
        break;
    case NativeDSPMemoryEncoding::AXParameterBlocks:
        if (bytes%320==0) return;
        break;
    case NativeDSPMemoryEncoding::AXStudio:
        if (bytes==120) return;
        break;
    default: throw std::invalid_argument("DSP memory encoding is unknown");
    }
    throw std::invalid_argument("DSP native field layout has an incomplete wire record");
}
bool NativeLittleEndian() {
    const std::uint16_t one=1;
    return *reinterpret_cast<const unsigned char*>(&one)!=0;
}
// Offset of a device byte in its actual native CPU field. No unaligned native
// integer access occurs, including AXSTUDIO's packed four-byte fields.
std::size_t NativeByteOffset(NativeDSPMemoryEncoding encoding,std::size_t wire) {
    if (!NativeLittleEndian()) return wire;
    switch (encoding) {
    case NativeDSPMemoryEncoding::NativeU16: return wire^std::size_t(1);
    case NativeDSPMemoryEncoding::NativeU32: return wire^std::size_t(3);
    case NativeDSPMemoryEncoding::AXParameterBlocks: {
        const auto field=wire%320;
        if (field>=12 && field<16) return wire^std::size_t(3);
        if (field<296) return wire^std::size_t(1);
        return wire; // Original opaque padding bytes, not native integers.
    }
    case NativeDSPMemoryEncoding::AXStudio: {
        const auto field=wire%6,record=wire-field;
        return field<4 ? record+3-field : record+9-field;
    }
    case NativeDSPMemoryEncoding::RawBytes: return wire;
    default: throw std::logic_error("Live DSP memory pin has an invalid encoding");
    }
}
// Device byte i of a transfer at wire offset is native byte NativeByteOffset(wire+i)
// of the pin. Every field mapping is its own inverse, so the same index maps a
// write. These loops produce exactly the bytes of the per-byte form when the
// transfer buffer does not alias the pin; callers keep that form otherwise.
template<bool Write>
void ConvertFields(NativeDSPMemoryEncoding encoding,unsigned char* native,std::size_t wire,
                   unsigned char* device,std::size_t bytes) {
    const auto move=[&](std::size_t i,std::size_t n) {
        if constexpr (Write) native[n]=device[i]; else device[i]=native[n];
    };
    if (!NativeLittleEndian() || encoding==NativeDSPMemoryEncoding::RawBytes) {
        for (std::size_t i=0;i<bytes;++i) move(i,wire+i);
        return;
    }
    switch (encoding) {
    case NativeDSPMemoryEncoding::NativeU16:
        for (std::size_t i=0;i<bytes;++i) move(i,(wire+i)^std::size_t(1));
        return;
    case NativeDSPMemoryEncoding::NativeU32:
        for (std::size_t i=0;i<bytes;++i) move(i,(wire+i)^std::size_t(3));
        return;
    case NativeDSPMemoryEncoding::AXParameterBlocks:
        if (wire%320==0 && bytes%320==0) {
            // Whole parameter blocks: u16 fields, the u32 at 12..15, opaque tail.
            for (std::size_t record=0;record<bytes;record+=320) {
                const auto at=wire+record;
                for (std::size_t k=0;k<12;++k) move(record+k,(at+k)^std::size_t(1));
                for (std::size_t k=12;k<16;++k) move(record+k,(at+k)^std::size_t(3));
                for (std::size_t k=16;k<296;++k) move(record+k,(at+k)^std::size_t(1));
                for (std::size_t k=296;k<320;++k) move(record+k,at+k);
            }
            return;
        }
        for (std::size_t i=0,field=wire%320;i<bytes;++i) {
            const auto at=wire+i;
            move(i,field>=12 && field<16 ? at^std::size_t(3) : field<296 ? at^std::size_t(1) : at);
            if (++field==320) field=0;
        }
        return;
    case NativeDSPMemoryEncoding::AXStudio:
        for (std::size_t i=0,field=wire%6;i<bytes;++i) {
            const auto record=wire+i-field;
            move(i,field<4 ? record+3-field : record+9-field);
            if (++field==6) field=0;
        }
        return;
    default: throw std::logic_error("Live DSP memory pin has an invalid encoding");
    }
}
bool Disjoint(const void* a,std::size_t aBytes,const void* b,std::size_t bBytes) {
    const auto x=reinterpret_cast<std::uintptr_t>(a),y=reinterpret_cast<std::uintptr_t>(b);
    return x+aBytes<=y || y+bBytes<=x;
}
NativeDSPMemoryPin Pin(AddressSpace& state,const void* address,std::size_t bytes,bool writable,
                       NativeDSPMemoryEncoding encoding=NativeDSPMemoryEncoding::RawBytes) {
    if (!bytes || bytes>std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("DSP source pin has no representable positive byte extent");
    RequireEncoding(encoding,bytes);
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
            reinterpret_cast<std::uintptr_t>(address),retained,encoding});
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
NativeDSPMemoryPin PinNativeDSPMemory(const void* address,std::size_t bytes,bool writable,
                                   NativeDSPMemoryEncoding encoding) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireOwner(state);
    return Pin(state,address,bytes,writable,encoding);
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
void DSPBackendValidateMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,std::size_t bytes,bool writing) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    NativePointer(state,address,bytes,writing);
}
namespace {
void ReadLocked(const AddressSpace& state,std::uint32_t address,void* destination,std::size_t bytes) {
    if (!destination && bytes) throw std::invalid_argument("DSP read destination is null");
    const auto it=FindSpan(state,address,bytes,false);
    auto* source=SpanPointer(it,address);
    const auto& span=it->second;
    if (span.encoding==NativeDSPMemoryEncoding::RawBytes) {
        if (bytes) std::memcpy(destination,source,bytes);
    } else {
        const auto offset=static_cast<std::size_t>(address-it->first);
        auto* base=reinterpret_cast<unsigned char*>(span.native_address);
        auto* output=static_cast<unsigned char*>(destination);
        if (Disjoint(output,bytes,base,span.bytes))
            ConvertFields<false>(span.encoding,base,offset,output,bytes);
        else // Aliasing transfer: keep the historical ascending byte order.
            for (std::size_t i=0;i<bytes;++i) output[i]=base[NativeByteOffset(span.encoding,offset+i)];
    }
}
}
void DSPBackendReadMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,void* destination,std::size_t bytes) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    ReadLocked(state,address,destination,bytes);
}
unsigned char DSPBackendReadSampleByte(NativeDSPMemoryEndpoint endpoint,std::uint32_t address) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    auto it=state.spans.upper_bound(address);
    if (it!=state.spans.begin()) {
        --it;
        const auto offset=static_cast<std::uint64_t>(address)-it->first;
        if (offset<it->second.bytes) {
            unsigned char value{};ReadLocked(state,address,&value,1);return value;
        }
        // The accelerator reads whatever physical memory follows a sample
        // allocation when a retail end address overruns it (one ADPCM frame at
        // most). Such bytes are not pinned; read the actual SDK backing there.
        if (it->second.encoding==NativeDSPMemoryEncoding::RawBytes &&
            offset-it->second.bytes<NativeDSPSampleOverrunBytes)
            return *static_cast<const volatile unsigned char*>(OSPhysicalToCached(address));
    }
    char detail[160];
    std::snprintf(detail,sizeof(detail),"DSP sample read is outside its pinned allocation and overrun (address=0x%08x)",address);
    throw std::out_of_range(detail);
}
std::size_t DSPBackendReadSampleBytes(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,
                                      unsigned char* destination,std::size_t capacity) {
    if (!destination || !capacity) throw std::invalid_argument("DSP sample window is empty");
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    auto it=state.spans.upper_bound(address);
    if (it!=state.spans.begin()) {
        --it;
        const auto offset=static_cast<std::uint64_t>(address)-it->first;
        if (offset<it->second.bytes) {
            const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(capacity,it->second.bytes-offset));
            ReadLocked(state,address,destination,count);
            return count;
        }
        if (it->second.encoding==NativeDSPMemoryEncoding::RawBytes &&
            offset-it->second.bytes<NativeDSPSampleOverrunBytes) {
            destination[0]=*static_cast<const volatile unsigned char*>(OSPhysicalToCached(address));
            return 1;
        }
    }
    char detail[160];
    std::snprintf(detail,sizeof(detail),"DSP sample read is outside its pinned allocation and overrun (address=0x%08x)",address);
    throw std::out_of_range(detail);
}
void DSPBackendWriteMemory(NativeDSPMemoryEndpoint endpoint,std::uint32_t address,const void* source,std::size_t bytes) {
    auto& state=State();std::lock_guard lock(state.mutex);RequireDevice(state,endpoint);
    if (!source && bytes) throw std::invalid_argument("DSP write source is null");
    const auto it=FindSpan(state,address,bytes,true);
    auto* destination=SpanPointer(it,address);
    const auto& span=it->second;
    if (span.encoding==NativeDSPMemoryEncoding::RawBytes) {
        if (bytes) std::memcpy(destination,source,bytes);
    } else {
        const auto offset=static_cast<std::size_t>(address-it->first);
        auto* base=reinterpret_cast<unsigned char*>(span.native_address);
        const auto* input=static_cast<const unsigned char*>(source);
        if (Disjoint(input,bytes,base,span.bytes))
            ConvertFields<true>(span.encoding,base,offset,const_cast<unsigned char*>(input),bytes);
        else // Aliasing transfer: keep the historical ascending byte order.
            for (std::size_t i=0;i<bytes;++i) base[NativeByteOffset(span.encoding,offset+i)]=input[i];
    }
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
