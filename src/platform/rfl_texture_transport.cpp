#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error RFL texture transport must share the original module ownership registry
#endif
#include "platform/rfl_texture_transport.h"
#include "platform/game_allocation_ownership.h"
#include <RVLFaceLib/RFLi_Texture.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

static_assert(sizeof(RFLiTexture)==32 && offsetof(RFLiTexture,width)==2
              && offsetof(RFLiTexture,height)==4 && offsetof(RFLiTexture,imageOfs)==28);

namespace {
using namespace mscharged::platform;
GameCompletedSpan Serialized(const void* source,std::size_t bytes) {
    GameCompletedSpan result;
    if (!bytes || !FindGameCompletedSpan(source,bytes,result)
        || FindGameByteDomain(source,bytes)!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RFL texture input requires exact completed Wii serialized bytes");
    return result;
}
}

extern "C" std::uint16_t ChargedReadRFLTextureHalf(const void* source) {
    Serialized(source,2);
    const auto* p=static_cast<const unsigned char*>(source);
    return (std::uint16_t(p[0])<<8)|p[1];
}

extern "C" void ChargedCopyRFLTextureImage(void* destination,const void* texture,std::size_t bytes) {
    const auto header=Serialized(texture,sizeof(RFLiTexture));
    const auto* p=static_cast<const unsigned char*>(texture);
    const auto* word=p+offsetof(RFLiTexture,imageOfs);
    const auto offset=(std::uint32_t(word[0])<<24)|(std::uint32_t(word[1])<<16)
        |(std::uint32_t(word[2])<<8)|word[3];
    const auto address=reinterpret_cast<std::uintptr_t>(texture);
    const auto base=reinterpret_cast<std::uintptr_t>(header.base);
    if (address<base || address-base>header.bytes
        || offset>header.bytes-(address-base)
        || bytes>header.bytes-(address-base)-offset
        || offset>std::numeric_limits<std::uintptr_t>::max()-address)
        throw std::invalid_argument("RFL texture image offset/count escaped its real completed source");
    const auto* image=reinterpret_cast<const void*>(address+offset);
    if (bytes) {
        const auto payload=Serialized(image,bytes);
        if (payload.base!=header.base || payload.bytes!=header.bytes
            || payload.allocation.incarnation!=header.allocation.incarnation)
            throw std::invalid_argument("RFL texture image crosses a completed source identity");
        GameMemoryStorageSpan storage;
        GameAllocationSpan allocation;
        if (!FindGameMemoryStorage(destination,bytes,storage)) {
            GameHeapMetadataSpan heap;
            if (FindGameHeapMetadataSource(destination,1,heap))
                throw std::invalid_argument("RFL texture copy destination is unrepresented MEM capacity");
            if (!FindGameAllocationSpan(destination,bytes,allocation))
                throw std::invalid_argument("RFL texture copy destination has no exact live owner extent");
        }
        GameByteWriteReservation written(destination,bytes);
        if (!written.Tracked())
            throw std::invalid_argument("RFL texture copy destination has no completed-write reservation");
        std::memcpy(destination,image,bytes);
        written.Complete(GameByteDomain::WiiSerialized);
    } else {
        std::memcpy(destination,image,bytes);
    }
}

static_assert(offsetof(RFLiTexture,lodBias)==26);

extern "C" std::int16_t ChargedReadRFLTextureSignedHalf(const void* source) {
    const auto value=ChargedReadRFLTextureHalf(source);
    return value<0x8000 ? static_cast<std::int16_t>(value)
        : static_cast<std::int16_t>(std::int32_t(value)-0x10000);
}

extern "C" void* ChargedBorrowRFLTextureImage(const void* texture) {
    const auto header=Serialized(texture,sizeof(RFLiTexture));
    const auto* p=static_cast<const unsigned char*>(texture);
    const auto* word=p+offsetof(RFLiTexture,imageOfs);
    const auto offset=(std::uint32_t(word[0])<<24)|(std::uint32_t(word[1])<<16)
        |(std::uint32_t(word[2])<<8)|word[3];
    const auto address=reinterpret_cast<std::uintptr_t>(texture);
    const auto base=reinterpret_cast<std::uintptr_t>(header.base);
    if (address<base || address-base>header.bytes
        || offset>=header.bytes-(address-base)
        || offset>std::numeric_limits<std::uintptr_t>::max()-address)
        throw std::invalid_argument("RFL borrowed texture image offset escaped completed source");
    auto* image=reinterpret_cast<void*>(address+offset);
    const auto payload=Serialized(image,header.bytes-(address-base)-offset);
    if (payload.base!=header.base || payload.bytes!=header.bytes
        || payload.allocation.incarnation!=header.allocation.incarnation)
        throw std::invalid_argument("RFL borrowed texture image crosses completed source identity");
    return image;
}
