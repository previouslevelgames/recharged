#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error RFL shape transport must share the original module ownership registry
#endif
#include "platform/rfl_shape_transport.h"
#include "platform/game_allocation_ownership.h"
#include <dolphin/gx.h>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

extern "C" void RFLiTransformCoordinate(std::int16_t*, const std::int16_t*);

namespace {
using namespace mscharged::platform;
void Serialized(const void* source, std::size_t bytes) {
    GameCompletedSpan span;
    if (!bytes || !FindGameCompletedSpan(source,bytes,span)
        || FindGameByteDomain(source,bytes)!=GameByteDomain::WiiSerialized)
        throw std::invalid_argument("RFL shape read requires an actual completed serialized span; MEM temp completion is not inferred");
}
std::uint16_t Half(const void* source) {
    Serialized(source,2);
    const auto* p=static_cast<const unsigned char*>(source);
    return (std::uint16_t(p[0])<<8)|p[1];
}
}

extern "C" std::uint16_t ChargedReadRFLShapeHalf(const void* source) {
    return Half(source);
}
extern "C" std::int16_t ChargedReadRFLShapeS16(const void* source) {
    return std::bit_cast<std::int16_t>(Half(source));
}
extern "C" void ChargedCopyRFLShapeVector(void* destination,const void* source) {
    Serialized(source,12);
    const auto* p=static_cast<const unsigned char*>(source);
    std::uint32_t words[3];
    for (unsigned i=0;i<3;++i) {
        const auto* at=p+i*4;
        words[i]=(std::uint32_t(at[0])<<24)|(std::uint32_t(at[1])<<16)
            |(std::uint32_t(at[2])<<8)|at[3];
    }
    // Original Vec destination/lifetime belongs to the calling source. Float
    // payload bits are retained, including NaN/Inf; no policy/normalization.
    std::memcpy(destination,words,sizeof(words));
}
extern "C" void ChargedCopyRFLShapeBytes(void* destination,const void* source,std::size_t bytes) {
    if (bytes) Serialized(source,bytes);
    // Preserve original memcpy and Wii texcoord bytes. No MEM parent or output
    // publication is invented; generated array ownership is a separate hold.
    std::memcpy(destination,source,bytes);
}
extern "C" void ChargedTransformRFLShapeCoordinate(std::int16_t* destination,const std::int16_t* source) {
    Serialized(source,6);
    const auto* p=reinterpret_cast<const unsigned char*>(source);
    const std::int16_t native[3]={
        std::bit_cast<std::int16_t>(Half(p)),
        std::bit_cast<std::int16_t>(Half(p+2)),
        std::bit_cast<std::int16_t>(Half(p+4))};
    // Invoke the genuine unchanged original permutation/sign decisions.
    RFLiTransformCoordinate(destination,native);
}
extern "C" void ChargedSetRFLGraphicsArray(int attribute,const void* data,std::uint8_t stride) {
    const auto span=ResolveGameGraphicsArray(data);
    if (span.bytes>std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Qualified RFL GX array extent exceeds canonical u32 size");
    GXSetArray(static_cast<GXAttr>(attribute),data,static_cast<std::uint32_t>(span.bytes),stride,span.little_endian);
}
