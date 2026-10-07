#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error RFL serialized transport must share the original module's owner registry
#endif
#include "platform/rfl_resource_transport.h"
#include "platform/game_allocation_ownership.h"
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
using namespace mscharged::platform;
static_assert(sizeof(std::uint16_t)==2 && sizeof(std::uint32_t)==4);

void Serialized(const void* pointer, std::size_t bytes) {
    GameCompletedSpan span;
    if (!bytes || !FindGameCompletedSpan(pointer,bytes,span)
        || FindGameByteDomain(pointer,bytes)!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RFL numeric bytes lack an actual completed Wii serialized source read");
}
void CacheRange(const void* cache,std::uint32_t size,const void* field,std::size_t bytes) {
    GameCompletedSpan span;
    if (!size || !FindGameCompletedSpan(cache,size,span))
        throw std::invalid_argument("RFL supplied cache size exceeds its actual completed original owner");
    const auto base=reinterpret_cast<std::uintptr_t>(cache);
    const auto address=reinterpret_cast<std::uintptr_t>(field);
    if (address<base || address-base>size || bytes>size-(address-base))
        throw std::invalid_argument("RFL cached bytes exceed the original supplied resource length");
}
std::uint16_t Half(const void* pointer) {
    Serialized(pointer,2);
    const auto* p=static_cast<const unsigned char*>(pointer);
    return (std::uint16_t(p[0])<<8)|p[1];
}
std::uint32_t Word(const void* pointer) {
    Serialized(pointer,4);
    const auto* p=static_cast<const unsigned char*>(pointer);
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)
        |(std::uint32_t(p[2])<<8)|p[3];
}
const void* Address(const void* cache,std::uint32_t size,std::uint32_t offset,std::size_t bytes) {
    CacheRange(cache,size,cache,0);
    if (offset>size || bytes>size-offset)
        throw std::invalid_argument("RFL cached offset exceeds the source resource length");
    const auto base=reinterpret_cast<std::uintptr_t>(cache);
    if (base>std::numeric_limits<std::uintptr_t>::max()-offset)
        throw std::invalid_argument("RFL cached address overflows native pointer width");
    return reinterpret_cast<const void*>(base+offset);
}
}

extern "C" std::uint16_t ChargedReadRFLCachedHalf(const void* cache,std::uint32_t size,const void* word) {
    CacheRange(cache,size,word,2);return Half(word);
}
extern "C" std::uint32_t ChargedReadRFLCachedWord(const void* cache,std::uint32_t size,const void* word) {
    CacheRange(cache,size,word,4);return Word(word);
}
extern "C" void* ChargedRFLCachedAddress(const void* cache,std::uint32_t size,std::uint32_t offset,std::size_t alignment) {
    const auto* pointer=Address(cache,size,offset,0);
    if (!alignment || reinterpret_cast<std::uintptr_t>(pointer)%alignment)
        throw std::invalid_argument("RFL cached table is not aligned for its original native scalar pointer");
    return const_cast<void*>(pointer);
}
extern "C" void ChargedCopyRFLCachedBytes(void* destination,const void* cache,
    std::uint32_t size,std::uint32_t offset,std::uint32_t bytes) {
    const auto* source=Address(cache,size,offset,bytes);
    if (bytes) Serialized(source,bytes);
    // Publish only the original successful copy into a known live source MEM
    // child. Unknown buffers retain441's raw copy semantics and gain no data
    // completion; no parent work/capacity or generated array is published.
    GameMemoryStorageSpan storage;
    GameByteWriteReservation publication;
    if (bytes && FindGameMemoryStorage(destination,bytes,storage))
        publication=GameByteWriteReservation(destination,bytes);
    std::memcpy(destination,source,bytes);
    if (publication.Tracked()) publication.Complete(GameByteDomain::WiiSerialized);
}
extern "C" std::uint16_t ChargedReadRFLNANDHalf(const void* word) { return Half(word); }
extern "C" std::uint32_t ChargedReadRFLNANDWord(const void* word) { return Word(word); }
