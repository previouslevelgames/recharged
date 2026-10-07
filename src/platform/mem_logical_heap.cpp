#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error MEM metadata must share the original game allocation registry
#endif
#include "platform/mem_logical_heap.h"
#include "platform/game_allocation_ownership.h"
#include <revolution/mem/expHeap.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
using namespace mscharged::platform;
namespace {
// OwnedDOL/MWCC measured geometry. Native struct fields remain full width.
constexpr std::size_t WiiHeapBytes=60+20,WiiBlockBytes=16,WiiAlignment=4;
struct HeaderPair { MEMiHeapHead common; MEMiExpHeapHead exp; };
static_assert(offsetof(HeaderPair,exp)==sizeof(MEMiHeapHead));
static_assert(sizeof(HeaderPair)==sizeof(MEMiHeapHead)+sizeof(MEMiExpHeapHead));
static_assert(sizeof(HeaderPair)%alignof(MEMiExpHeapMBlockHead)==0);
std::size_t Slots(const GameHeapMetadataSpan& span) { return span.source_bytes/WiiAlignment; }
MEMiExpHeapMBlockHead* Blocks(const GameHeapMetadataSpan& span) {
    return reinterpret_cast<MEMiExpHeapMBlockHead*>(static_cast<unsigned char*>(span.data)+sizeof(HeaderPair));
}
unsigned char* Initialized(const GameHeapMetadataSpan& span) {
    return reinterpret_cast<unsigned char*>(Blocks(span)+Slots(span));
}
GameHeapMetadataSpan Heap(const void* owner) {
    GameHeapMetadataSpan span;
    if(!FindGameHeapMetadata(owner,span))throw std::invalid_argument("MEM heap is unknown or retired");
    return span;
}
std::size_t LogicalIndex(const GameHeapMetadataSpan& span,const void* header) {
    const auto base=reinterpret_cast<std::uintptr_t>(span.source),address=reinterpret_cast<std::uintptr_t>(header);
    if(address<base || address-base<WiiHeapBytes || address-base>span.source_bytes-WiiBlockBytes
        || (address-base)%WiiAlignment)
        throw std::invalid_argument("MEM block header leaves its exact logical region");
    return (address-base)/WiiAlignment;
}
std::size_t NativeIndex(const GameHeapMetadataSpan& span,const void* block) {
    const auto base=reinterpret_cast<std::uintptr_t>(Blocks(span)),address=reinterpret_cast<std::uintptr_t>(block);
    if(address<base || (address-base)%sizeof(MEMiExpHeapMBlockHead)
        || (address-base)/sizeof(MEMiExpHeapMBlockHead)>=Slots(span))
        throw std::invalid_argument("MEM block has no actual native metadata slot");
    const auto index=(address-base)/sizeof(MEMiExpHeapMBlockHead);
    if(!Initialized(span)[index])throw std::invalid_argument("MEM block slot was never initialized by the source");
    return index;
}
}
extern "C" void* ChargedCreateMEMExpHeapMetadata(void* source,std::uint32_t bytes) {
    if(bytes<WiiHeapBytes+WiiBlockBytes+4 || (bytes%WiiAlignment)
        || (reinterpret_cast<std::uintptr_t>(source)%WiiAlignment))
        throw std::invalid_argument("MEM metadata requires the original aligned positive heap region");
    const auto slots=bytes/WiiAlignment;
    constexpr auto perSlot=sizeof(MEMiExpHeapMBlockHead)+1;
    if(slots>(std::numeric_limits<std::size_t>::max()-sizeof(HeaderPair))/perSlot)
        throw std::overflow_error("Native MEM metadata extent overflows");
    GameHeapMetadataReservation reservation(source,bytes,sizeof(HeaderPair)+slots*perSlot);
    void* data=reservation.Data();
    new(data) HeaderPair; // Real source InitHeapHead/InitExpHeap writes fields.
    auto* blocks=reinterpret_cast<MEMiExpHeapMBlockHead*>(static_cast<unsigned char*>(data)+sizeof(HeaderPair));
    // Slot initialization is a host storage fact, not a source readiness flag.
    std::memset(reinterpret_cast<unsigned char*>(blocks+slots),0,slots);
    reservation.Commit(data);
    return data;
}
extern "C" void* ChargedMEMHeapAddress(const void* owner) {
    GameHeapMetadataSpan span;
    // Common code also owns other genuine MEM heap types. Only registered
    // projected Exp heaps replace the physical address with their source base.
    return FindGameHeapMetadata(owner,span)?const_cast<void*>(span.source):const_cast<void*>(owner);
}
extern "C" void ChargedValidateMEMHeap(const void* owner) { (void)Heap(owner); }
extern "C" void* ChargedInitMEMBlock(void* header) {
    GameHeapMetadataSpan span;
    if(!FindGameHeapMetadataSource(header,WiiBlockBytes,span))
        throw std::invalid_argument("MEM block initialization has no live logical heap");
    const auto index=LogicalIndex(span,header);
    auto* block=Blocks(span)+index;
    new(block) MEMiExpHeapMBlockHead;
    Initialized(span)[index]=1;
    return block;
}
extern "C" void* ChargedMEMBlockAddress(const void* block) {
    GameHeapMetadataSpan span;
    if(!FindGameHeapMetadataNative(block,sizeof(MEMiExpHeapMBlockHead),span))
        throw std::invalid_argument("MEM block is unknown or retired");
    const auto index=NativeIndex(span,block);
    return static_cast<unsigned char*>(const_cast<void*>(span.source))+index*WiiAlignment;
}
extern "C" void* ChargedLookupMEMBlock(const void* owner,void* header) {
    const auto span=Heap(owner);const auto index=LogicalIndex(span,header);
    if(!Initialized(span)[index])throw std::invalid_argument("MEM payload has no original initialized header");
    return Blocks(span)+index;
}
extern "C" void ChargedValidateMEMFree(const void* owner,const void* block) {
    const auto span=Heap(owner);const auto index=NativeIndex(span,block);
    const auto* value=static_cast<const MEMiExpHeapMBlockHead*>(block);
    const auto logical=reinterpret_cast<std::uintptr_t>(span.source)+index*WiiAlignment;
    const auto margin=value->attribute.fields.alignment;
    ValidateGameHeapMetadataRangeRetirement(owner,reinterpret_cast<const void*>(logical-margin),
        WiiBlockBytes+std::size_t(value->blockSize)+margin);
}
extern "C" void ChargedValidateMEMDestroy(const void* owner) { ValidateGameHeapMetadataRetirement(owner); }
extern "C" void ChargedRetireMEMHeap(const void* owner) { RetireGameHeapMetadata(owner); }
